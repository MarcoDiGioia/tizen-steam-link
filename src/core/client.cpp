#include "core/client.h"

#include <cstring>

#if !defined(__EMSCRIPTEN__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

#include "core/log.h"

extern "C" {
#include "client/client_pri.h"
}

namespace tsl {
namespace {

Host MakeHost(const IHS_HostInfo *info) {
    Host host;
    host.info = *info;
    return host;
}

}  // namespace

Client::Client(const Identity &identity, Listener *listener)
    : config_(identity.ClientConfig()), listener_(listener) {
    client_ = IHS_ClientCreate(&config_);
    IHS_ClientSetLogFunction(client_, IhsLogFunction());
    static const IHS_ClientDiscoveryCallbacks discovery = {Discovered};
    static const IHS_ClientAuthorizationCallbacks authorization = {AuthProgress, AuthSuccess, AuthFailed};
    static const IHS_ClientStreamingCallbacks streaming = {StreamProgress, StreamSuccess, StreamFailed};
    IHS_ClientSetDiscoveryCallbacks(client_, &discovery, this);
    IHS_ClientSetAuthorizationCallbacks(client_, &authorization, this);
    IHS_ClientSetStreamingCallbacks(client_, &streaming, this);
}

Client::~Client() {
    IHS_ClientStopDiscovery(client_);
    IHS_ClientStop(client_);
    IHS_ClientThreadedJoin(client_);
    IHS_ClientDestroy(client_);
}

bool Client::StartDiscovery(uint32_t intervalMs) { return IHS_ClientStartDiscovery(client_, intervalMs); }

void Client::StopDiscovery() { IHS_ClientStopDiscovery(client_); }

bool Client::Probe(const std::string &ip, uint16_t port) {
    IHS_SocketAddress address{};
    if (!IHS_IPAddressFromString(&address.ip, ip.c_str())) {
        Log(IHS_LogLevelWarn, "Client", "Not an IP address: %s", ip.c_str());
        return false;
    }
    address.port = port;
    // Same message the discovery timer broadcasts (client/discovery.c), sent to one host.
    CMsgRemoteClientBroadcastDiscovery discovery = CMSG_REMOTE_CLIENT_BROADCAST_DISCOVERY__INIT;
    IHS_BaseLock(&client_->base);
    uint32_t seq = client_->discoverySeq++;
    IHS_BaseUnlock(&client_->base);
    discovery.has_seq_num = true;
    discovery.seq_num = seq;
    return IHS_ClientSend(client_, address, k_ERemoteClientBroadcastMsgDiscovery,
                          reinterpret_cast<ProtobufCMessage *>(&discovery));
}

int Client::ProbeLocalBroadcasts() {
    int sent = 0;
#if !defined(__EMSCRIPTEN__)
    struct ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return 0;
    for (struct ifaddrs *ifa = list; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_BROADCAST) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        if (ifa->ifa_broadaddr == nullptr) continue;
        char buf[INET_ADDRSTRLEN];
        const auto *addr = reinterpret_cast<const struct sockaddr_in *>(ifa->ifa_broadaddr);
        if (inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf)) != nullptr && Probe(buf)) sent++;
    }
    freeifaddrs(list);
#endif
    return sent;
}

bool Client::Pair(const Host &host, const std::string &pin) {
    return IHS_ClientAuthorizationRequest(client_, &host.info, pin.c_str());
}

void Client::CancelPair() { IHS_ClientAuthorizationCancel(client_); }

bool Client::RequestStream(const Host &host, const StreamRequest &request, const HostKey &key) {
    // The streaming request and the session key in its answer use the per-host secret.
    IHS_ClientSetSecretKey(client_, key.data());
    IHS_StreamingRequest req;
    std::memset(&req, 0, sizeof(req));
    req.streamingEnable.video = true;
    req.streamingEnable.audio = true;
    req.streamingEnable.input = true;
    req.maxResolution.x = request.maxWidth;
    req.maxResolution.y = request.maxHeight;
    req.audioChannelCount = request.audioChannels;
    req.streamingInterface = request.interface;
    return IHS_ClientStreamingRequest(client_, &host.info, &req);
}

void Client::Discovered(IHS_Client *, const IHS_HostInfo *host, void *context) {
    static_cast<Client *>(context)->listener_->OnHostFound(MakeHost(host));
}

void Client::AuthProgress(IHS_Client *, const IHS_HostInfo *host, void *context) {
    static_cast<Client *>(context)->listener_->OnPairProgress(MakeHost(host));
}

void Client::AuthSuccess(IHS_Client *client, const IHS_HostInfo *host, uint64_t steamId, void *context) {
    // ihslib has switched the client to the key negotiated with this host by now.
    HostKey key;
    IHS_ClientGetSecretKey(client, key.data());
    static_cast<Client *>(context)->listener_->OnPairSucceeded(MakeHost(host), steamId, key);
}

void Client::AuthFailed(IHS_Client *, const IHS_HostInfo *host, IHS_AuthorizationResult result, void *context) {
    static_cast<Client *>(context)->listener_->OnPairFailed(MakeHost(host), result);
}

void Client::StreamProgress(IHS_Client *, const IHS_HostInfo *host, void *context) {
    static_cast<Client *>(context)->listener_->OnStreamProgress(MakeHost(host));
}

void Client::StreamSuccess(IHS_Client *, const IHS_HostInfo *host, const IHS_SocketAddress *address,
                           const uint8_t *sessionKey, size_t sessionKeyLen, void *context) {
    IHS_SessionInfo info;
    std::memset(&info, 0, sizeof(info));
    info.address = *address;
    if (sessionKeyLen > sizeof(info.sessionKey)) {
        sessionKeyLen = sizeof(info.sessionKey);
    }
    std::memcpy(info.sessionKey, sessionKey, sessionKeyLen);
    info.sessionKeyLen = sessionKeyLen;
    static_cast<Client *>(context)->listener_->OnStreamReady(MakeHost(host), info);
}

void Client::StreamFailed(IHS_Client *, const IHS_HostInfo *host, IHS_StreamingResult result, void *context) {
    static_cast<Client *>(context)->listener_->OnStreamFailed(MakeHost(host), result);
}

const char *AuthorizationResultName(IHS_AuthorizationResult result) {
    switch (result) {
        case IHS_AuthorizationSuccess: return "success";
        case IHS_AuthorizationDenied: return "denied on the host";
        case IHS_AuthorizationNotLoggedIn: return "Steam is not logged in on the host";
        case IHS_AuthorizationOffline: return "host is offline";
        case IHS_AuthorizationBusy: return "host is busy";
        case IHS_AuthorizationInProgress: return "in progress";
        case IHS_AuthorizationTimedOut: return "timed out";
        case IHS_AuthorizationFailed: return "failed";
        case IHS_AuthorizationCanceled: return "canceled";
    }
    return "unknown";
}

const char *StreamingResultName(IHS_StreamingResult result) {
    switch (result) {
        case IHS_StreamingSuccess: return "success";
        case IHS_StreamingUnauthorized: return "this device is not paired with the host";
        case IHS_StreamingScreenLocked: return "the host screen is locked";
        case IHS_StreamingFailed: return "failed";
        case IHS_StreamingBusy: return "host is busy";
        case IHS_StreamingInProgress: return "in progress";
        case IHS_StreamingCanceled: return "canceled";
        case IHS_StreamingDriversNotInstalled: return "streaming drivers are not installed on the host";
        case IHS_StreamingDisabled: return "Remote Play is disabled on the host";
        case IHS_StreamingBroadcastingActive: return "host is broadcasting";
        case IHS_StreamingVRActive: return "VR is active on the host";
        case IHS_StreamingPINRequired: return "host requires a PIN";
        case IHS_StreamingTransportUnavailable: return "transport unavailable";
        case IHS_StreamingInvisible: return "host is set to invisible";
        case IHS_StreamingGameLaunchFailed: return "game launch failed";
        case IHS_StreamingTimeout: return "timed out";
    }
    return "unknown";
}

}  // namespace tsl

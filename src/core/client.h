#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "core/identity.h"
#include "core/ihs.h"

namespace tsl {

// Steam pairs with a key exchange, so the secret a device streams with is negotiated per host.
using HostKey = std::array<uint8_t, 32>;

struct Host {
    IHS_HostInfo info{};

    std::string hostname() const { return info.hostname; }
    std::string address() const { return IpToString(info.address.ip); }
};

struct StreamRequest {
    int maxWidth = 1920;
    int maxHeight = 1080;
    int audioChannels = 2;
    IHS_StreamInterface interface = IHS_StreamInterfaceDefault;
};

// Everything that happens before a stream: finding hosts, pairing, asking for a stream.
// Listener methods run on ihslib's client thread.
class Client {
public:
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void OnHostFound(const Host &host) { (void) host; }
        virtual void OnPairProgress(const Host &host) { (void) host; }
        // key is what this host agreed on; store it with the host, RequestStream needs it.
        virtual void OnPairSucceeded(const Host &host, uint64_t steamId, const HostKey &key) {
            (void) host, (void) steamId, (void) key;
        }
        virtual void OnPairFailed(const Host &host, IHS_AuthorizationResult result) { (void) host, (void) result; }
        virtual void OnStreamProgress(const Host &host) { (void) host; }
        virtual void OnStreamReady(const Host &host, const IHS_SessionInfo &session) { (void) host, (void) session; }
        virtual void OnStreamFailed(const Host &host, IHS_StreamingResult result) { (void) host, (void) result; }
    };

    static constexpr uint16_t kDiscoveryPort = 27036;

    // identity must outlive the client.
    Client(const Identity &identity, Listener *listener);
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // Broadcast every intervalMs (0 = once).
    bool StartDiscovery(uint32_t intervalMs);
    void StopDiscovery();

    // Unicast discovery packet to one address. Steam answers with the same status
    // message as for a broadcast, which ends up in OnHostFound. A subnet broadcast address
    // (192.168.1.255) works too.
    bool Probe(const std::string &ip, uint16_t port = kDiscoveryPort);

    // Discovery to the broadcast address of every local IPv4 network. ihslib's own broadcast
    // goes to 255.255.255.255, which a Linux Steam host ignored in the ihslib fork's tests
    // while answering the subnet address. Uses getifaddrs(), so it does nothing in the WASM
    // build; the TV app probes its own subnet instead.
    int ProbeLocalBroadcasts();

    // The PIN has to be typed into the dialog Steam opens on the host.
    bool Pair(const Host &host, const std::string &pin);
    void CancelPair();

    // key is the one stored when pairing with this host.
    bool RequestStream(const Host &host, const StreamRequest &request, const HostKey &key);

private:
    static void Discovered(IHS_Client *client, const IHS_HostInfo *host, void *context);
    static void AuthProgress(IHS_Client *client, const IHS_HostInfo *host, void *context);
    static void AuthSuccess(IHS_Client *client, const IHS_HostInfo *host, uint64_t steamId, void *context);
    static void AuthFailed(IHS_Client *client, const IHS_HostInfo *host, IHS_AuthorizationResult result,
                           void *context);
    static void StreamProgress(IHS_Client *client, const IHS_HostInfo *host, void *context);
    static void StreamSuccess(IHS_Client *client, const IHS_HostInfo *host, const IHS_SocketAddress *address,
                              const uint8_t *sessionKey, size_t sessionKeyLen, void *context);
    static void StreamFailed(IHS_Client *client, const IHS_HostInfo *host, IHS_StreamingResult result,
                             void *context);

    IHS_ClientConfig config_;
    Listener *listener_;
    IHS_Client *client_;
};

const char *AuthorizationResultName(IHS_AuthorizationResult result);
const char *StreamingResultName(IHS_StreamingResult result);

}  // namespace tsl

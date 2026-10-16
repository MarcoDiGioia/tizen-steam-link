/*
 * UDP backend for ihslib (ihs_udp.h), used instead of its platforms/ihs_udp_posix.c.
 *
 * Two differences, both for the Tizen Sockets Extension:
 *  - No fcntl(): Samsung's Emscripten aborts on fcntl(F_SETFL) for sockets, and ihslib's
 *    client calls IHS_UDPSocketSetBlocking() on startup. Sockets stay blocking.
 *  - Receive timeouts use poll() instead of SO_RCVTIMEO. Sockets without an explicit
 *    timeout (the session's) still wake up every DEFAULT_TIMEOUT_MS, so the receive loop
 *    notices an interrupt without waiting for the next packet.
 *  - Socket errors (ECONNREFUSED and the like) count as "no data", not as a dead socket.
 *    So do poll() errors: only an invalid descriptor ends the receive loop. ihslib's loop
 *    exits for good on -1, and nothing tells the app.
 *  - Sockets are bound (0.0.0.0, any port) as soon as they are created, as Moonlight for
 *    Tizen does with its UDP sockets. ihslib starts polling before its first send, and
 *    Linux only binds a socket implicitly on that send.
 *  - Problems are reported through TSL_PlatformLogHook (set by the app's logging) instead
 *    of being dropped.
 *  - Desktop builds only: TSL_TEST_RECV_LOSS=<percent> drops that share of incoming
 *    datagrams of LOSS_MIN_BYTES or more (video; audio and control messages are smaller),
 *    to reproduce Wi-Fi packet loss on a wired network.
 *
 * Address conversion is the same as ihslib's POSIX backend.
 */
#include "ihs_udp.h"
#include "ihs_buffer.h"
#include "ihs_thread.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_TIMEOUT_MS 50
/* After a poll() error, wait this long before trying again instead of spinning. */
#define ERROR_BACKOFF_US 20000

/* Set by the app (src/core/log.cpp). NULL means problems go to stderr. */
void (*TSL_PlatformLogHook)(int warning, const char *message) = NULL;

static void PlatformLog(int warning, const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (TSL_PlatformLogHook != NULL) {
        TSL_PlatformLogHook(warning, buf);
    } else {
        fprintf(stderr, "[UDP] %s\n", buf);
    }
}

struct IHS_UDPSocket {
    int fd;
    int timeoutMs;
    IHS_Mutex *mutex;
    unsigned pollErrors;
    unsigned recvErrors;
    unsigned sendErrors;
    bool received;
};

#ifndef __EMSCRIPTEN__
#define LOSS_MIN_BYTES 1000
static pthread_once_t lossOnce = PTHREAD_ONCE_INIT;
static double lossPercent = 0;

static void LossInit(void) {
    const char *value = getenv("TSL_TEST_RECV_LOSS");
    if (value != NULL) lossPercent = atof(value);
    if (lossPercent > 0) {
        PlatformLog(1, "Test hook: dropping %.2f%% of incoming datagrams of %d bytes or more", lossPercent,
                    LOSS_MIN_BYTES);
    }
}

/* Logs the session header (type, retransmit count, channel, fragment, packet id). */
static void LossLog(const char *what, const uint8_t *data, ssize_t len) {
    if (len < 13) return;
    PlatformLog(1, "Test hook: %s %zd bytes: type %u retransmit %u channel %u fragment %d packet %u", what, len,
                data[0] & 0x7f, data[1], data[4], (int16_t) (data[5] | data[6] << 8), data[7] | data[8] << 8);
}

/* Recently dropped packets (channel, fragment and packet id), to notice when one comes back.
 * FEC shards of one message share the packet id and differ in the fragment id. */
#define LOSS_RECENT 64
static __thread uint64_t lossRecent[LOSS_RECENT];
static __thread unsigned lossRecentCount;

static bool LossDrop(const uint8_t *data, ssize_t len) {
    pthread_once(&lossOnce, LossInit);
    if (lossPercent <= 0 || len < 13) return false;
    unsigned type = data[0] & 0x7f;
    /* Session packets only: discovery datagrams start with 0xff. */
    if (type >= 16) return false;
    static __thread uint16_t seenTypes;
    if (type > 10 && !(seenTypes & (1u << type))) {
        seenTypes |= (uint16_t) (1u << type);
        LossLog("first packet of an unknown type", data, len);
    }
    uint64_t key = (uint64_t) data[4] << 32 | (uint64_t) (data[5] | data[6] << 8) << 16 | (uint64_t) (data[7] | data[8] << 8);
    for (unsigned i = 0; i < LOSS_RECENT && i < lossRecentCount; i++) {
        if (lossRecent[i] == key && type != 7 && type != 8) {
            lossRecent[i] = UINT64_MAX;
            LossLog("dropped earlier, arrived again", data, len);
        }
    }
    if (data[1] > 0) LossLog("retransmitted", data, len);
    if (len < LOSS_MIN_BYTES) return false;
    static __thread unsigned seed = 1;
    if (rand_r(&seed) >= lossPercent / 100.0 * RAND_MAX) return false;
    lossRecent[lossRecentCount++ % LOSS_RECENT] = key;
    LossLog("dropped", data, len);
    return true;
}
#endif

static void AddressFromSys(IHS_SocketAddress *ihs, const struct sockaddr_storage *sys);

static socklen_t AddressToSys(const IHS_SocketAddress *ihs, struct sockaddr_storage *sys);

IHS_UDPSocket *IHS_UDPSocketOpen(bool broadcast) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        PlatformLog(1, "socket() failed: errno %d", errno);
        return NULL;
    }
    IHS_UDPSocket *s = calloc(1, sizeof(IHS_UDPSocket));
    s->fd = fd;
    s->timeoutMs = DEFAULT_TIMEOUT_MS;
    s->mutex = IHS_MutexCreate();
    if (broadcast) {
        int opt = 1;
        if (setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)) != 0) {
            PlatformLog(1, "SO_BROADCAST failed: errno %d", errno);
        }
    }
    struct sockaddr_in any;
    memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    any.sin_port = 0;
    if (bind(s->fd, (struct sockaddr *) &any, sizeof(any)) != 0) {
        PlatformLog(1, "bind(0.0.0.0:0) failed: errno %d", errno);
    } else {
        struct sockaddr_in local;
        socklen_t localLen = sizeof(local);
        if (getsockname(s->fd, (struct sockaddr *) &local, &localLen) == 0) {
            PlatformLog(0, "UDP socket %d bound to port %u%s", s->fd, ntohs(local.sin_port),
                        broadcast ? " (broadcast)" : "");
        }
    }
    return s;
}

void IHS_UDPSocketClose(IHS_UDPSocket *s) {
    IHS_MutexDestroy(s->mutex);
    close(s->fd);
    free(s);
}

int IHS_UDPSocketReceive(IHS_UDPSocket *s, IHS_UDPPacket *packet) {
    struct pollfd pfd = {.fd = s->fd, .events = POLLIN, .revents = 0};
    int ready = poll(&pfd, 1, s->timeoutMs);
    if (ready == 0) {
        return 0;
    }
    if (ready < 0) {
        int err = errno;
        if (err == EBADF) {
            PlatformLog(1, "poll() on socket %d: bad descriptor, receiving stops", s->fd);
            return -1;
        }
        if (err != EINTR && s->pollErrors++ % 100 == 0) {
            PlatformLog(1, "poll() on socket %d failed: errno %d (%u so far)", s->fd, err, s->pollErrors);
        }
        usleep(ERROR_BACKOFF_US);
        return 0;
    }
    if (pfd.revents & POLLNVAL) {
        PlatformLog(1, "poll() on socket %d: invalid descriptor, receiving stops", s->fd);
        return -1;
    }
    if ((pfd.revents & POLLERR) && !(pfd.revents & POLLIN)) {
        /* A pending socket error, such as ICMP port unreachable after sending to a computer
         * where Steam isn't running. Read it to clear it; the socket is still usable, and
         * returning -1 would end ihslib's receive loop for good. */
        int error = 0;
        socklen_t errorLen = sizeof(error);
        getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &errorLen);
        return 0;
    }
    struct sockaddr_storage sender;
    socklen_t senderLen = sizeof(sender);
    IHS_BufferEnsureMaxSize(&packet->buffer, 2048);
    ssize_t len = recvfrom(s->fd, IHS_BufferPointer(&packet->buffer), IHS_BufferMaxSize(&packet->buffer), 0,
                           (struct sockaddr *) &sender, &senderLen);
    if (len < 0) {
        int err = errno;
        if (err == EBADF || err == ENOTSOCK) {
            PlatformLog(1, "recvfrom() on socket %d: errno %d, receiving stops", s->fd, err);
            return -1;
        }
        if (s->recvErrors++ % 100 == 0) {
            PlatformLog(1, "recvfrom() on socket %d failed: errno %d (%u so far)", s->fd, err, s->recvErrors);
        }
        return 0;
    }
#ifndef __EMSCRIPTEN__
    if (LossDrop(IHS_BufferPointer(&packet->buffer), len)) {
        return 0;
    }
#endif
    if (!s->received) {
        s->received = true;
        PlatformLog(0, "UDP socket %d: first packet received", s->fd);
    }
    packet->buffer.size = (size_t) len;
    AddressFromSys(&packet->address, &sender);
    return 1;
}

bool IHS_UDPSocketSend(IHS_UDPSocket *s, const IHS_UDPPacket *packet) {
    struct sockaddr_storage addr;
    socklen_t addrLen = AddressToSys(&packet->address, &addr);
    if (addrLen == 0) {
        return false;
    }
    IHS_MutexLock(s->mutex);
    bool ret = sendto(s->fd, IHS_BufferPointer(&packet->buffer), packet->buffer.size, 0, (struct sockaddr *) &addr,
                      addrLen) > 0;
    int err = errno;
    if (!ret && s->sendErrors++ % 100 == 0) {
        PlatformLog(1, "sendto() on socket %d failed: errno %d (%u so far)", s->fd, err, s->sendErrors);
    }
    IHS_MutexUnlock(s->mutex);
    return ret;
}

bool IHS_UDPSocketSetBlocking(IHS_UDPSocket *s, bool blocking) {
    (void) s;
    /* Sockets are created blocking and stay that way; receive waits are bounded by poll(). */
    return blocking;
}

bool IHS_UDPSocketSetRecvTimeout(IHS_UDPSocket *s, uint32_t timeoutUs) {
    int ms = (int) (timeoutUs / 1000);
    s->timeoutMs = ms > 0 ? ms : 1;
    return true;
}

static void AddressFromSys(IHS_SocketAddress *ihs, const struct sockaddr_storage *sys) {
    memset(ihs, 0, sizeof(*ihs));
    switch (sys->ss_family) {
        case AF_INET: {
            const struct sockaddr_in *addr = (const struct sockaddr_in *) sys;
            ihs->ip.family = IHS_IPAddressFamilyIPv4;
            memcpy(&ihs->ip.v4.data, &addr->sin_addr, 4);
            ihs->port = ntohs(addr->sin_port);
            break;
        }
        case AF_INET6: {
            const struct sockaddr_in6 *addr = (const struct sockaddr_in6 *) sys;
            ihs->ip.family = IHS_IPAddressFamilyIPv6;
            memcpy(&ihs->ip.v6.data, &addr->sin6_addr, 16);
            ihs->port = ntohs(addr->sin6_port);
            break;
        }
        default:
            break;
    }
}

static socklen_t AddressToSys(const IHS_SocketAddress *ihs, struct sockaddr_storage *sys) {
    switch (ihs->ip.family) {
        case IHS_IPAddressFamilyIPv4: {
            struct sockaddr_in *addr = (struct sockaddr_in *) sys;
            memset(addr, 0, sizeof(struct sockaddr_in));
            addr->sin_family = AF_INET;
            addr->sin_port = htons(ihs->port);
            memcpy(&addr->sin_addr, ihs->ip.v4.data, 4);
            return sizeof(*addr);
        }
        case IHS_IPAddressFamilyIPv6: {
            struct sockaddr_in6 *addr = (struct sockaddr_in6 *) sys;
            memset(addr, 0, sizeof(struct sockaddr_in6));
            addr->sin6_family = AF_INET6;
            addr->sin6_port = htons(ihs->port);
            memcpy(&addr->sin6_addr, ihs->ip.v6.data, 16);
            return sizeof(*addr);
        }
        default:
            return 0;
    }
}

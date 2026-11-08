/* MCPX network card (guest FEF00000), the nForce MAC the game's XNET drives itself: registers, descriptor
 * rings and interrupt. Its Ethernet frames go over UDP to a hub (RECOMP_FCI_NET=<host>:<port>[/<room>],
 * tools/fci_hub.py) or straight to peer cards (RECOMP_FCI_NET_PEERS); without either the link is down, as
 * with no cable. research/docs/multiplayer.md */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmio_decode.h"

#define NIC_BASE   0xFEF00000u
#define NIC_SIZE   0x1000u /* one page; the card's BAR is 1 KiB */
#define NIC_VECTOR 4       /* bus level 4, HalGetInterruptVector (00522D7F) */

/* registers, as XNET uses them (00522C3D its DPC, 005223CA stop, 00522EBB..0052304D bring-up) */
#define R_IRQ_STATUS 0x000u /* write 1 to clear */
#define R_IRQ_MASK   0x004u
#define R_TX_STATUS  0x088u /* bit 0 busy */
#define R_RX_CONTROL 0x094u /* bit 0 receiving */
#define R_RX_STATUS  0x098u
#define R_TX_RING    0x100u /* physical addresses of the descriptor rings */
#define R_RX_RING    0x104u
#define R_RING_SIZES 0x108u /* receive entries - 1 high, transmit entries - 1 low (Evolution XNET 003D64BA) */
#define R_TXRX_CONTROL 0x144u
#define R_MII_STATUS   0x180u /* write 1 to clear */
#define TXRX_KICK      0x01u  /* frames queued to send */
#define TXRX_RX_KICK   0x02u  /* receive descriptors handed back after running out */
#define TXRX_IDLE_REQ  0x04u  /* stop request, answered by TXRX_IDLE */
#define TXRX_IDLE      0x08u
#define TXRX_RESET     0x10u
#define IRQ_RX         0x02u
#define IRQ_RX_NOBUF   0x04u
#define IRQ_TX_OK      0x10u

/* descriptors: buffer address, then length - 1 in the low half and flags in the high half */
#define D_OWNED   0x80000000u /* the card's: a frame to send, or a buffer to fill */
#define D_TX_LAST 0x00010000u /* the frame's last fragment */
#define D_RX_DONE 0x00010000u /* filled; the low half is the frame's length */

#define LINK_UP   0x0Bu /* XNET_ETHERNET_LINK_ACTIVE | 100MBPS | FULL_DUPLEX */
#define FRAME_MAX 1536
#define RX_QUEUE  64
#define ETHER_MAX 1514 /* the largest frame without its checksum */
#define MAGIC  "FCN1"  /* hub datagrams: magic, kind ('H' hello: MAC, cookie, room; 'C' cookie; 'F' frame) */
#define COOKIE 8       /* the hub's proof that this address is ours (tools/fci_hub.py) */

static uint32_t regs[NIC_SIZE / 4];
static SRWLOCK s_lock = SRWLOCK_INIT; /* the timer thread may tick before init */
static ptrdiff_t s_mem_offset;
static uint32_t s_tx, s_rx; /* the card's place in each ring */
static int s_tx_kick, s_rx_kick;
static int s_rx_starved; /* the driver had no buffer for the next frame */
static SOCKET s_sock = INVALID_SOCKET;
#define PEERS_MAX 32
typedef struct {
    struct sockaddr_in addr;
    uint8_t mac[6]; /* the card at that endpoint, from its first frame */
    int has_mac;
} peer_t;
static peer_t s_peers[PEERS_MAX];
static int s_npeers, s_direct; /* s_direct: frames go to s_peers, not to a hub */
static unsigned long s_unrouted, s_stray;
static char s_room[33];
static uint8_t s_cookie[COOKIE];
static uint8_t s_mac[6];
static int s_mac_made;
static uint8_t s_rxq[RX_QUEUE][FRAME_MAX];
static int s_rxq_len[RX_QUEUE], s_rxq_head, s_rxq_count;
static unsigned long s_sent, s_received, s_dropped;
static int s_trace
        = -1; /* RECOMP_FCI_NET_TRACE=1: control writes and interrupts (the first 400) and frames */
static unsigned s_traced;

#define TRACE(...)                                                                                           \
    do {                                                                                                     \
        if (s_trace > 0 && s_traced++ < 400)                                                                 \
            fprintf(stderr, "[FCI] nic: " __VA_ARGS__), fflush(stderr);                                      \
    } while (0)
#define TRACE_FRAME(...)                                                                                     \
    do {                                                                                                     \
        if (s_trace > 0)                                                                                     \
            fprintf(stderr, "[FCI] nic: " __VA_ARGS__), fflush(stderr);                                      \
    } while (0)

extern uint32_t xbox_ContiguousAllocatedBytes(void); /* xbox_memory_layout.h */
extern int xbox_IrqlBlocksInterrupts(void);          /* kernel.h */

#define R(off) regs[(off) / 4]

static void fail(const char *what, uint32_t a)
{
    fprintf(stderr, "[FCI] STOP 94 network card: %s (%08X)\n", what, a);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 94);
}

/* a DMA address as the card sees it, as the APU model reads them (apu_core.c mcpx_apu_phys) */
static uint8_t *phys(uint32_t a, uint32_t len)
{
    uint32_t va;
    a &= 0x0FFFFFFFu;
    if ((a & 0x0C000000u) == 0x08000000u) /* main RAM, tagged by MmGetPhysicalAddress */
        va = a & 0x03FFFFFFu;
    else if (a < xbox_ContiguousAllocatedBytes())
        va = 0x80000000u + a;
    else
        va = a & 0x03FFFFFFu;
    if ((va & 0x03FFFFFFu) + len > 0x04000000u)
        fail("a descriptor or buffer outside memory", a);
    return (uint8_t *)s_mem_offset + va;
}

static uint32_t *desc(uint32_t ring, uint32_t i) { return (uint32_t *)phys(ring + i * 8, 8); }

void fci_nic_mac(uint8_t *mac)
{
    if (!s_mac_made) {
        /* locally administered, so it clashes with no real card; one per process for several on one PC */
        LARGE_INTEGER t;
        uint32_t h = GetCurrentProcessId() * 2654435761u;
        QueryPerformanceCounter(&t);
        h ^= (uint32_t)t.QuadPart * 40503u;
        s_mac[0] = 0x02, s_mac[1] = 0x46, s_mac[2] = 0x43;
        s_mac[3] = (uint8_t)(h >> 16), s_mac[4] = (uint8_t)(h >> 8), s_mac[5] = (uint8_t)h;
        s_mac_made = 1;
    }
    memcpy(mac, s_mac, 6);
}

ULONG fci_nic_link_state(void) { return s_sock != INVALID_SOCKET ? LINK_UP : 0; }

/* Down before a relaunch (fci_manual.c): the next instance re-reads RECOMP_FCI_NET_PEERS and would find
 * this one's port still bound. The process is going, so no other state needs winding back. */
void fci_nic_shutdown(void)
{
    AcquireSRWLockExclusive(&s_lock);
    if (s_sock != INVALID_SOCKET) {
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
    }
    ReleaseSRWLockExclusive(&s_lock);
}

/* to the hub (to == NULL) or to one peer; a lost datagram is a lost frame, as on a wire */
static void send_datagram_to(const peer_t *to, char kind, const void *data, int len)
{
    char buf[FRAME_MAX + 8];
    memcpy(buf, MAGIC, 4);
    buf[4] = kind;
    memcpy(buf + 5, data, (size_t)len);
    if (to)
        sendto(s_sock, buf, len + 5, 0, (const struct sockaddr *)&to->addr, sizeof to->addr);
    else
        send(s_sock, buf, len + 5, 0);
}

static void send_datagram(char kind, const void *data, int len) { send_datagram_to(NULL, kind, data, len); }

/* a DHCP request (UDP to port 67): no server answers on a peer link, as on the hub; XNET picks its own address */
static int is_dhcp(const uint8_t *f, uint32_t len)
{
    uint32_t ihl;
    if (len < 14 + 20 + 8 || f[12] != 0x08 || f[13] != 0x00 || f[23] != 17)
        return 0;
    ihl = (f[14] & 15u) * 4;
    return 14 + ihl + 4 <= len && f[14 + ihl + 2] == 0 && f[14 + ihl + 3] == 67;
}

/* a frame from the guest: unicast to the peer whose card it names, broadcast to every peer */
static void route_frame(const uint8_t *f, uint32_t len)
{
    int i;
    if (!s_direct) {
        send_datagram('F', f, (int)len);
        return;
    }
    if (f[0] & 1) {
        if (is_dhcp(f, len))
            return;
        for (i = 0; i < s_npeers; i++)
            send_datagram_to(&s_peers[i], 'F', f, (int)len);
        return;
    }
    for (i = 0; i < s_npeers; i++)
        if (s_peers[i].has_mac && !memcmp(s_peers[i].mac, f, 6)) {
            send_datagram_to(&s_peers[i], 'F', f, (int)len);
            return;
        }
    if (!s_unrouted++) /* XNET answers only cards it heard from, so this is unexpected but harmless */
        fprintf(stderr, "[FCI] network: a frame to a card no peer has shown, dropped\n"), fflush(stderr);
}

/* a datagram from no peer, or not its card's frame: dropped, so nothing outside the session reaches XNET */
static void stray(const struct sockaddr_in *from)
{
    if (!s_stray++)
        fprintf(stderr, "[FCI] network: a datagram from %s:%u is not a peer's frame, dropped (logged once)\n",
                inet_ntoa(from->sin_addr), ntohs(from->sin_port)),
                fflush(stderr);
}

static peer_t *peer_at(const struct sockaddr_in *a)
{
    int i;
    for (i = 0; i < s_npeers; i++)
        if (s_peers[i].addr.sin_addr.s_addr == a->sin_addr.s_addr && s_peers[i].addr.sin_port == a->sin_port)
            return &s_peers[i];
    return NULL;
}

static void hello(void)
{
    char b[6 + COOKIE + sizeof s_room];
    size_t n = strlen(s_room);
    uint8_t mac[6];
    fci_nic_mac(mac);
    memcpy(b, mac, 6);
    memcpy(b + 6, s_cookie, COOKIE);
    memcpy(b + 6 + COOKIE, s_room, n);
    send_datagram('H', b, (int)(6 + COOKIE + n));
}

/* send every complete frame the driver has queued; it marks a frame's last fragment before its first */
static void tx_run(void)
{
    uint32_t ring = R(R_TX_RING), n = (R(R_RING_SIZES) & 0xFFFFu) + 1, k;
    if (!ring)
        return;
    for (k = 0; k < n; k++) {
        uint8_t frame[FRAME_MAX];
        uint32_t i = s_tx % n, len = 0, parts = 0, j;
        if (!(desc(ring, i)[1] & D_OWNED))
            return;
        for (;;) {
            uint32_t *d = desc(ring, (i + parts) % n), seg = (d[1] & 0xFFFFu) + 1;
            if (!(d[1] & D_OWNED) || ++parts > n)
                return;                /* not handed over whole yet */
            if (len + seg > ETHER_MAX) /* XNET's limit too (090 <- 5EE with the checksum) */
                fail("a frame longer than Ethernet allows", len + seg);
            memcpy(frame + len, phys(d[0], seg), seg);
            len += seg;
            if (d[1] & D_TX_LAST)
                break;
        }
        for (j = 0; j < parts; j++)
            desc(ring, (i + j) % n)[1] &= ~D_OWNED;
        s_tx = (i + parts) % n;
        if (len < 60) { /* the card pads short frames */
            memset(frame + len, 0, 60 - len);
            len = 60;
        }
        TRACE_FRAME("tx %u bytes in %u descriptors from %u\n", len, parts, i);
        if (s_sock != INVALID_SOCKET) {
            route_frame(frame, len);
            if (!s_sent++)
                fprintf(stderr, "[FCI] network: first frame sent (%u bytes)\n", len), fflush(stderr);
        } else if (!s_dropped++) {
            fprintf(stderr, "[FCI] network: a frame sent with no link, dropped as by a card with no cable\n");
            fflush(stderr);
        }
        R(R_IRQ_STATUS) |= IRQ_TX_OK;
    }
}

/* fill the driver's receive buffers from the queue */
static void rx_run(void)
{
    uint32_t ring = R(R_RX_RING), n = (R(R_RING_SIZES) >> 16) + 1;
    if (!ring || !(R(R_RX_CONTROL) & 1))
        return;
    while (s_rxq_count) {
        uint32_t *d = desc(ring, s_rx % n), cap, len = (uint32_t)s_rxq_len[s_rxq_head];
        if (!(d[1] & D_OWNED)) {
            if (!s_rx_starved) { /* once per stall, or it storms while frames wait */
                TRACE_FRAME("rx: no buffer at %u, %d frames waiting\n", s_rx % n, s_rxq_count);
                R(R_IRQ_STATUS) |= IRQ_RX_NOBUF;
                s_rx_starved = 1;
            }
            return;
        }
        s_rx_starved = 0;
        cap = (d[1] & 0xFFFFu) + 1;
        if (len > cap)
            len = cap;
        memcpy(phys(d[0], len), s_rxq[s_rxq_head], len);
        d[1] = D_RX_DONE | len;
        TRACE_FRAME("rx %u bytes into %u\n", len, s_rx % n);
        s_rx = (s_rx + 1) % n;
        s_rxq_head = (s_rxq_head + 1) % RX_QUEUE;
        s_rxq_count--;
        R(R_IRQ_STATUS) |= IRQ_RX;
        if (!s_received++)
            fprintf(stderr, "[FCI] network: first frame received (%u bytes)\n", len), fflush(stderr);
    }
}

static void receive(void)
{
    char buf[FRAME_MAX + 8];
    int n, k;
    for (k = 0; k < 4 * RX_QUEUE; k++) { /* bounded: the rest waits for the next millisecond */
        int slot;
        struct sockaddr_in from;
        int fromlen = sizeof from;
        peer_t *p = NULL;
        n = recvfrom(s_sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
        if (n == SOCKET_ERROR) {
            int e = WSAGetLastError(); /* an oversized datagram or an ICMP report: skip it, not the rest */
            if (e == WSAEMSGSIZE || e == WSAECONNRESET)
                continue;
            return;
        }
        if (n < 5 || memcmp(buf, MAGIC, 4))
            continue;
        if (s_direct) { /* only peers reach the guest, each with its own card's frames */
            p = fromlen == sizeof from ? peer_at(&from) : NULL;
            if (!p || buf[4] != 'F') {
                stray(&from);
                continue;
            }
        } else if (buf[4] == 'C' && n == 5 + COOKIE) { /* the hub's cookie: say hello with it now */
            memcpy(s_cookie, buf + 5, COOKIE);
            hello();
            continue;
        }
        if (buf[4] != 'F' || n < 5 + 14 || n > 5 + ETHER_MAX)
            continue;
        if (p) {
            const uint8_t *src = (const uint8_t *)buf + 5 + 6;
            if (src[0] & 1) { /* no card sends from a group address */
                stray(&from);
                continue;
            }
            if (!p->has_mac) {
                memcpy(p->mac, src, 6);
                p->has_mac = 1;
                fprintf(stderr, "[FCI] network: peer %s:%u is card %02X:%02X:%02X:%02X:%02X:%02X\n",
                        inet_ntoa(from.sin_addr), ntohs(from.sin_port), src[0], src[1], src[2], src[3],
                        src[4], src[5]);
                fflush(stderr);
            } else if (memcmp(p->mac, src, 6)) {
                stray(&from);
                continue;
            }
        }
        if (s_rxq_count == RX_QUEUE) { /* the driver is not keeping up: the oldest goes, as a full FIFO */
            s_rxq_head = (s_rxq_head + 1) % RX_QUEUE;
            s_rxq_count--;
        }
        slot = (s_rxq_head + s_rxq_count) % RX_QUEUE;
        memcpy(s_rxq[slot], buf + 5, (size_t)(n - 5));
        s_rxq_len[slot] = n - 5;
        s_rxq_count++;
    }
}

/* from the kernel's 1 ms timer thread (runtime patch 0026): the vector to raise, or -1 */
int fci_nic_tick(void)
{
    static ULONGLONG next_hello;
    int raise;
    AcquireSRWLockExclusive(&s_lock);
    if (s_sock != INVALID_SOCKET) {
        ULONGLONG now = GetTickCount64();
        if (!s_direct && now >= next_hello) {
            hello();
            next_hello = now + 2000;
        }
        receive();
    }
    if (s_tx_kick) {
        s_tx_kick = 0;
        tx_run();
    }
    if (s_rxq_count || s_rx_kick) {
        s_rx_kick = 0;
        rx_run();
    }
    /* level triggered: the routine masks the card, and its DPC acknowledges before unmasking */
    raise = (R(R_IRQ_STATUS) & R(R_IRQ_MASK)) && !xbox_IrqlBlocksInterrupts();
    if (raise)
        TRACE("raise: status %02X mask %02X\n", R(R_IRQ_STATUS), R(R_IRQ_MASK));
    ReleaseSRWLockExclusive(&s_lock);
    return raise ? NIC_VECTOR : -1;
}

static uint64_t nic_read(void *dev, uint32_t off, int size)
{
    uint64_t v = 0;
    (void)dev;
    AcquireSRWLockExclusive(&s_lock);
    memcpy(&v, (uint8_t *)regs + off, (size_t)size);
    ReleaseSRWLockExclusive(&s_lock);
    return v;
}

static void nic_write(void *dev, uint32_t off, uint64_t val, int size)
{
    uint32_t v = (uint32_t)val, reg = off & ~3u;
    (void)dev;
    AcquireSRWLockExclusive(&s_lock);
    if (size != 4 && (reg == R_IRQ_STATUS || reg == R_TXRX_CONTROL || reg == R_MII_STATUS))
        fail("a partial write to a control register", off);
    if (reg == R_IRQ_STATUS || reg == R_IRQ_MASK || reg == R_TXRX_CONTROL || reg < 0x10
        || (reg >= 0x80 && reg < 0x100))
        TRACE("%03X <- %08X\n", off, v);
    switch (reg) {
    case R_IRQ_STATUS:
    case R_MII_STATUS:
        R(reg) &= ~v;
        break;
    case R_TX_STATUS:
    case R_RX_STATUS:
        R(reg) = v & ~1u; /* never busy: frames move between ticks */
        break;
    case R_TXRX_CONTROL:
        if (v & TXRX_RESET)
            s_tx = s_rx = 0;
        s_tx_kick |= (v & TXRX_KICK) != 0;
        s_rx_kick |= (v & TXRX_RX_KICK) != 0;
        R(reg) = (v & ~(TXRX_KICK | TXRX_RX_KICK)) | (v & TXRX_IDLE_REQ ? TXRX_IDLE : 0);
        break;
    case R_TX_RING:
    case R_RX_RING:
    case R_RING_SIZES:
        if (reg != R_RX_RING)
            s_tx = 0;
        if (reg != R_TX_RING)
            s_rx = 0;
        R(reg) = v;
        break;
    default:
        memcpy((uint8_t *)regs + off, &val, (size_t)size);
    }
    ReleaseSRWLockExclusive(&s_lock);
}

/* RECOMP_FCI_NET=<host>:<port>[/<room>]; 0 on a bad value or a socket error */
static int connect_hub(const char *spec)
{
    char host[256];
    const char *colon = strrchr(spec, ':'), *slash = strchr(spec, '/');
    struct addrinfo hints, *ai = NULL;
    WSADATA wsa;
    u_long nb = 1;
    if (!colon || (size_t)(colon - spec) >= sizeof host || (slash && slash < colon))
        return 0;
    memcpy(host, spec, (size_t)(colon - spec));
    host[colon - spec] = 0;
    snprintf(s_room, sizeof s_room, "%s", slash ? slash + 1 : "");
    {
        char port[16];
        size_t pn = slash ? (size_t)(slash - colon - 1) : strlen(colon + 1);
        if (!pn || pn >= sizeof port)
            return 0;
        memcpy(port, colon + 1, pn);
        port[pn] = 0;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) || getaddrinfo(host, port, &hints, &ai) || !ai)
            return 0;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET || connect(s_sock, ai->ai_addr, (int)ai->ai_addrlen)
        || ioctlsocket(s_sock, FIONBIO, &nb)) {
        freeaddrinfo(ai);
        return 0;
    }
    freeaddrinfo(ai);
    return 1;
}

/* RECOMP_FCI_NET_PEERS=<local port>,<host>:<port>[,<host>:<port>...]: a fixed set of peer cards, no hub.
 * The step before hole punching fills the table from the host list (multiplayer.md, design detail).
 * 0 on a bad spec or a socket error, with the failure in err. */
static int connect_peers(const char *spec, char *err, int errlen)
{
    char list[1024], *tok, *next;
    struct sockaddr_in local;
    WSADATA wsa;
    u_long nb = 1;
    int port;
    if (strlen(spec) >= sizeof list) {
        snprintf(err, errlen, "spec too long");
        return 0;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        snprintf(err, errlen, "WSAStartup failed (%d)", WSAGetLastError());
        return 0;
    }
    strcpy(list, spec);
    next = strchr(list, ',');
    if (!next) {
        snprintf(err, errlen, "spec is not <local port>,<host>:<port>[,...]");
        return 0;
    }
    *next++ = 0;
    port = atoi(list);
    if (port <= 0 || port > 65535) {
        snprintf(err, errlen, "local port %s is not 1-65535", list);
        return 0;
    }
    for (tok = next; tok && *tok; tok = next) {
        char *colon;
        struct addrinfo hints, *ai = NULL;
        next = strchr(tok, ',');
        if (next)
            *next++ = 0;
        colon = strrchr(tok, ':');
        if (!colon) {
            snprintf(err, errlen, "peer %s is not <host>:<port>", tok);
            return 0;
        }
        if (s_npeers == PEERS_MAX) {
            snprintf(err, errlen, "more than %d peers", PEERS_MAX);
            return 0;
        }
        *colon = 0;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(tok, colon + 1, &hints, &ai) || !ai) {
            snprintf(err, errlen, "peer %s:%s does not resolve", tok, colon + 1);
            return 0;
        }
        memcpy(&s_peers[s_npeers++].addr, ai->ai_addr, sizeof(struct sockaddr_in));
        freeaddrinfo(ai);
    }
    if (!s_npeers) {
        snprintf(err, errlen, "no peers after the local port");
        return 0;
    }
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_port = htons((u_short)port);
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) {
        snprintf(err, errlen, "no socket (%d)", WSAGetLastError());
        return 0;
    }
    if (bind(s_sock, (struct sockaddr *)&local, sizeof local)) {
        int e = WSAGetLastError();
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        snprintf(err, errlen, "bind of port %d failed (%d)", port, e);
        return 0;
    }
    if (ioctlsocket(s_sock, FIONBIO, &nb)) {
        int e = WSAGetLastError();
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        snprintf(err, errlen, "non-blocking mode failed (%d)", e);
        return 0;
    }
    s_direct = 1;
    return 1;
}

/* returns 0 when the page cannot be trapped or RECOMP_FCI_NET(_PEERS) cannot be used */
int fci_nic_init(ptrdiff_t mem_offset)
{
    DWORD old;
    const char *net = getenv("RECOMP_FCI_NET"), *peers = getenv("RECOMP_FCI_NET_PEERS");
    const char *trace = getenv("RECOMP_FCI_NET_TRACE");
    uint8_t m[6];
    s_trace = trace && *trace == '1';
    s_mem_offset = mem_offset;
    if (net && *net && peers && *peers) {
        fprintf(stderr, "[FCI] RECOMP_FCI_NET and RECOMP_FCI_NET_PEERS are both set: one link at a time\n");
        return 0;
    }
    if (net && *net) {
        if (!connect_hub(net)) {
            fprintf(stderr, "[FCI] RECOMP_FCI_NET=%s: not <host>:<port>[/<room>], or no socket (%d)\n", net,
                    WSAGetLastError());
            return 0;
        }
        fci_nic_mac(m);
        fprintf(stderr, "[FCI] network: link up through hub %s, card %02X:%02X:%02X:%02X:%02X:%02X\n", net,
                m[0], m[1], m[2], m[3], m[4], m[5]);
        fflush(stderr);
    } else if (peers && *peers) {
        char err[128];
        if (!connect_peers(peers, err, sizeof err)) {
            fprintf(stderr, "[FCI] RECOMP_FCI_NET_PEERS=%s: %s\n", peers, err);
            return 0;
        }
        fci_nic_mac(m);
        fprintf(stderr, "[FCI] network: link up to %d peers, card %02X:%02X:%02X:%02X:%02X:%02X\n", s_npeers,
                m[0], m[1], m[2], m[3], m[4], m[5]);
        fflush(stderr);
    }
    return VirtualProtect((char *)mem_offset + NIC_BASE, NIC_SIZE, PAGE_NOACCESS, &old) != 0;
}

int fci_nic_owns(uint32_t va) { return va >= NIC_BASE && va < NIC_BASE + NIC_SIZE; }

/* an access the decoder cannot emulate is a stop: the card would be silently skipped */
int fci_nic_mmio(void *ctx, uint32_t va)
{
    if (mmio_emulate((PCONTEXT)ctx, va - NIC_BASE, NULL, nic_read, nic_write))
        return 1;
    fail("an access the decoder cannot emulate", va - NIC_BASE);
    return 0;
}

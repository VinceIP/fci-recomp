/* MCPX network card (guest FEF00000), the nForce MAC the game's XNET drives itself: registers, descriptor
 * rings and interrupt. Its Ethernet frames go over UDP to a hub (RECOMP_FCI_NET=<host>:<port>[/<room>],
 * tools/fci_hub.py), straight to peer cards (RECOMP_FCI_NET_PEERS), or through the internet host list
 * (RECOMP_FCI_NET_LIST, with hole punching and STUN); without any the link is down, as with no cable.
 * research/docs/multiplayer.md */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmio_decode.h"
#include "fci_net_worker.h"

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

/* LIST mode: the internet host list (RECOMP_FCI_NET_LIST) */
#define LIST_PEER_MAX    16
#define LIST_PENDING_MAX 8
#define PUNCH_PERIOD_MS  100
#define PUNCH_WINDOW_MS  3000
#define PEER_EXPIRE_MS   15000
#define STUN_PERIOD_MS   60000
#define STUN_MAGIC       0x2112A442u

typedef enum { LP_PENDING, LP_LIVE, LP_RELAY } lp_state;

typedef struct {
    struct sockaddr_in endpoint;
    uint8_t mac[6];
    uint8_t nonce[16];
    lp_state state;
    int relay;            /* the relay socket (s_relsock) when in the relay room; -1 otherwise */
    ULONGLONG next_punch; /* next punch deadline (GetTickCount64) */
    ULONGLONG punch_end;  /* stop punching, transition or drop */
    ULONGLONG last_frame; /* last frame received (expiry) */
} lpeer_t;

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

/* LIST mode state */
static int s_list; /* 1 when RECOMP_FCI_NET_LIST is in use */
static lpeer_t s_lpeers[LIST_PEER_MAX];
static int s_nlpeers;
static int s_lpending;                /* count of LP_PENDING peers (cap at LIST_PENDING_MAX) */
static struct sockaddr_in s_stun_ep;  /* the card's public endpoint (from STUN or _ENDPOINT) */
static int s_stun_known;              /* 1 when the endpoint is set */
static ULONGLONG s_stun_next;         /* next STUN send deadline */
static uint32_t s_stun_txn;           /* STUN transaction ID counter */
static struct sockaddr_in s_stun_srv; /* the STUN server address (resolved at init) */
static int s_stun_srv_valid;          /* 1 when the STUN server is resolved */
static char s_list_url[512];          /* the Worker base URL (for the worker thread, round B) */
static char s_relay_spec[256];        /* RECOMP_FCI_NET_RELAY, if set */
/* the relay fallback (RECOMP_FCI_NET_RELAY): the hub keys a member by address, so one socket per
 * relay room, opened on the first relay peer and closed when its peer expires */
#define RELAY_MAX       4
#define HELLO_PERIOD_MS 2000           /* the hub leaves a quiet member after 10 s */
static struct sockaddr_in s_relay_hub; /* the hub address, resolved from _RELAY at init */
static int s_relay_hub_valid;
static SOCKET s_relsock[RELAY_MAX]; /* a socket per relay room; INVALID_SOCKET = free */
static uint8_t s_relcookie[RELAY_MAX][COOKIE];
static char s_relroom[RELAY_MAX][33];   /* the room name: the peer's nonce in hex (32) */
static ULONGLONG s_relhello[RELAY_MAX]; /* next hello deadline per socket */
static volatile LONG s_probe; /* the game broadcast in LIST mode: the worker should fetch the list */

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
        /* RECOMP_FCI_MAC: the relaunch (fci_manual.c) passes the instance's MAC to its successor, so an
         * instance keeps it across its relaunches, as a console keeps its through a reboot */
        const char *env = getenv("RECOMP_FCI_MAC");
        int kept = env
                   && sscanf(env, "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx", &s_mac[0], &s_mac[1], &s_mac[2],
                             &s_mac[3], &s_mac[4], &s_mac[5])
                              == 6;
        if (env && !kept)
            fprintf(stderr, "[FCI] RECOMP_FCI_MAC=%s is not six bytes of hex: a new MAC\n", env),
                    fflush(stderr);
        if (!kept) {
            /* locally administered, so it clashes with no real card; one per process for several on one PC */
            LARGE_INTEGER t;
            uint32_t h = GetCurrentProcessId() * 2654435761u;
            QueryPerformanceCounter(&t);
            h ^= (uint32_t)t.QuadPart * 40503u;
            s_mac[0] = 0x02, s_mac[1] = 0x46, s_mac[2] = 0x43;
            s_mac[3] = (uint8_t)(h >> 16), s_mac[4] = (uint8_t)(h >> 8), s_mac[5] = (uint8_t)h;
        }
        s_mac_made = 1;
    }
    memcpy(mac, s_mac, 6);
}

/* the card's MAC as "02:46:43:xx:xx:xx" (for the worker's register and connect calls) */
void fci_nic_mac_str(char *buf, int buflen)
{
    uint8_t m[6];
    fci_nic_mac(m);
    snprintf(buf, (size_t)buflen, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* a frame to a relay room, over that room's hub socket (the hub socket is connected to the hub) */
static void relay_send_frame(int i, const uint8_t *f, uint32_t len)
{
    char buf[FRAME_MAX + 8];
    memcpy(buf, MAGIC, 4);
    buf[4] = 'F';
    memcpy(buf + 5, f, (size_t)len);
    send(s_relsock[i], buf, (int)len + 5, 0);
}

static void relay_socket_close(int i)
{
    if (s_relsock[i] != INVALID_SOCKET) {
        closesocket(s_relsock[i]); /* closing the socket leaves the room */
        s_relsock[i] = INVALID_SOCKET;
        s_relroom[i][0] = 0;
    }
}

ULONG fci_nic_link_state(void) { return s_sock != INVALID_SOCKET ? LINK_UP : 0; }

/* Down before a relaunch (fci_manual.c): the next instance re-reads RECOMP_FCI_NET_PEERS and would find
 * this one's port still bound. The process is going, so no other state needs winding back. */
void fci_nic_shutdown(void)
{
    int i;
    fci_net_worker_stop(); /* before the socket closes: the worker takes s_lock for list calls */
    AcquireSRWLockExclusive(&s_lock);
    if (s_sock != INVALID_SOCKET) {
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
    }
    for (i = 0; i < RELAY_MAX; i++)
        relay_socket_close(i); /* the relay rooms leave with the process */
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

/* a frame from the guest: unicast to the peer whose card it names, broadcast (DHCP included, as the hub
   relays it) to every peer; returns 1 if it went out, 0 if dropped */
static int route_frame(const uint8_t *f, uint32_t len)
{
    int i;
    if (s_list) {
        /* LIST mode: route to the peer table by MAC (live over the card socket, relay over its room) */
        if (f[0] & 1) {
            /* broadcast: to every live and relay peer; signal the worker to fetch the list */
            int sent = 0;
            for (i = 0; i < s_nlpeers; i++) {
                lpeer_t *p = &s_lpeers[i];
                if (p->state == LP_LIVE) {
                    char buf[FRAME_MAX + 8];
                    memcpy(buf, MAGIC, 4);
                    buf[4] = 'F';
                    memcpy(buf + 5, f, (size_t)len);
                    sendto(s_sock, buf, (int)len + 5, 0, (const struct sockaddr *)&p->endpoint,
                           sizeof p->endpoint);
                    sent = 1;
                } else if (p->state == LP_RELAY) {
                    relay_send_frame(p->relay, f, len);
                    sent = 1;
                }
            }
            InterlockedExchange(&s_probe, 1);
            return sent;
        }
        for (i = 0; i < s_nlpeers; i++) {
            lpeer_t *p = &s_lpeers[i];
            if ((p->state != LP_LIVE && p->state != LP_RELAY) || memcmp(p->mac, f, 6))
                continue;
            if (p->state == LP_LIVE) {
                char buf[FRAME_MAX + 8];
                memcpy(buf, MAGIC, 4);
                buf[4] = 'F';
                memcpy(buf + 5, f, (size_t)len);
                sendto(s_sock, buf, (int)len + 5, 0, (const struct sockaddr *)&p->endpoint,
                       sizeof p->endpoint);
            } else {
                relay_send_frame(p->relay, f, len);
            }
            return 1;
        }
        if (!s_unrouted++) /* XNET answers only cards it heard from, so this is unexpected but harmless */
            fprintf(stderr, "[FCI] network: a frame to a card no peer has shown, dropped\n"), fflush(stderr);
        return 0;
    }
    if (!s_direct) {
        send_datagram('F', f, (int)len);
        return 1;
    }
    if (f[0] & 1) {
        for (i = 0; i < s_npeers; i++)
            send_datagram_to(&s_peers[i], 'F', f, (int)len);
        return s_npeers > 0;
    }
    for (i = 0; i < s_npeers; i++)
        if (s_peers[i].has_mac && !memcmp(s_peers[i].mac, f, 6)) {
            send_datagram_to(&s_peers[i], 'F', f, (int)len);
            return 1;
        }
    if (!s_unrouted++) /* XNET answers only cards it heard from, so this is unexpected but harmless */
        fprintf(stderr, "[FCI] network: a frame to a card no peer has shown, dropped\n"), fflush(stderr);
    return 0;
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

/* --- LIST mode: STUN, punch, peer table --- */

/* send a STUN-5 binding request from the card socket; the response's XOR-MAPPED-ADDRESS is the
 * card's public endpoint (the NAT mapping the punches will open) */
static void stun_send(void)
{
    char buf[20];
    if (!s_stun_srv_valid)
        return;
    s_stun_txn++;
    /* type 0x0001 (binding request), length 0, magic, 12-byte transaction ID */
    memcpy(buf, "\x00\x01\x00\x00", 4);
    uint32_t m = htonl(STUN_MAGIC);
    memcpy(buf + 4, &m, 4);
    memset(buf + 8, 0, 12);
    buf[8] = (char)(s_stun_txn >> 24), buf[9] = (char)(s_stun_txn >> 16);
    buf[10] = (char)(s_stun_txn >> 8), buf[11] = (char)s_stun_txn;
    sendto(s_sock, buf, 20, 0, (const struct sockaddr *)&s_stun_srv, sizeof s_stun_srv);
}

/* a STUN binding response (type 0x0101): extract the mapped address. Accept only from the
 * STUN server and only with our last transaction ID. Walk the attributes: prefer
 * XOR-MAPPED-ADDRESS (0x0020), fall back to MAPPED-ADDRESS (0x0001). */
static int stun_recv(const char *buf, int n, const struct sockaddr_in *from)
{
    uint32_t m, txn;
    int off;
    if (n < 20)
        return 0;
    if (buf[0] != 0x01 || buf[1] != 0x01)
        return 0;
    memcpy(&m, buf + 4, 4);
    if (ntohl(m) != STUN_MAGIC)
        return 0;
    /* only from the STUN server we sent to */
    if (from->sin_addr.s_addr != s_stun_srv.sin_addr.s_addr || from->sin_port != s_stun_srv.sin_port)
        return 0;
    /* only our last transaction ID */
    memcpy(&txn, buf + 8, 4);
    if (txn != htonl(s_stun_txn))
        return 0;
    /* walk the attributes */
    off = 20;
    while (off + 4 <= n) {
        uint16_t type = (uint16_t)((((uint8_t)buf[off] << 8) | (uint8_t)buf[off + 1]));
        uint16_t len = (uint16_t)((((uint8_t)buf[off + 2] << 8) | (uint8_t)buf[off + 3]));
        if (off + 4 + (int)len > n)
            break;
        if (type == 0x0020 && len == 8) {
            /* XOR-MAPPED-ADDRESS */
            uint16_t port = (uint16_t)((((uint8_t)buf[off + 6] << 8) | (uint8_t)buf[off + 7]) ^ 0x2112);
            uint32_t addr;
            memcpy(&addr, buf + off + 8, 4);
            addr ^= STUN_MAGIC;
            s_stun_ep.sin_family = AF_INET;
            s_stun_ep.sin_port = htons(port);
            s_stun_ep.sin_addr.s_addr = addr;
            if (!s_stun_known) {
                s_stun_known = 1;
                fprintf(stderr, "[FCI] network: STUN endpoint %s:%u\n", inet_ntoa(s_stun_ep.sin_addr),
                        ntohs(s_stun_ep.sin_port));
                fflush(stderr);
            }
            return 1;
        }
        if (type == 0x0001 && len == 8) {
            /* MAPPED-ADDRESS (plain, not XOR) */
            uint16_t port = (uint16_t)((((uint8_t)buf[off + 6] << 8) | (uint8_t)buf[off + 7]));
            uint32_t addr;
            memcpy(&addr, buf + off + 8, 4);
            s_stun_ep.sin_family = AF_INET;
            s_stun_ep.sin_port = htons(port);
            s_stun_ep.sin_addr.s_addr = ntohl(addr);
            if (!s_stun_known) {
                s_stun_known = 1;
                fprintf(stderr, "[FCI] network: STUN endpoint (MAPPED) %s:%u\n",
                        inet_ntoa(s_stun_ep.sin_addr), ntohs(s_stun_ep.sin_port));
                fflush(stderr);
            }
            return 1;
        }
        off += 4 + (int)len;
    }
    return 0;
}

/* send a punch datagram (FCN1 'P' + 16-byte nonce) to a peer's endpoint */
static void punch_send(const lpeer_t *p)
{
    char buf[21];
    memcpy(buf, MAGIC, 4);
    buf[4] = 'P';
    memcpy(buf + 5, p->nonce, 16);
    sendto(s_sock, buf, 21, 0, (const struct sockaddr *)&p->endpoint, sizeof p->endpoint);
}

static void punch_answer_send(const lpeer_t *p)
{
    char buf[21];
    memcpy(buf, MAGIC, 4);
    buf[4] = 'A';
    memcpy(buf + 5, p->nonce, 16);
    sendto(s_sock, buf, 21, 0, (const struct sockaddr *)&p->endpoint, sizeof p->endpoint);
}

/* a received punch 'P' or answer 'A' (FCN1 + type + 16-byte nonce): a 'P' is answered with an 'A';
 * an 'A' is never answered. Either one, with a matching nonce and endpoint, moves a pending peer to live. */
static void punch_recv(const char *buf, char type, const struct sockaddr_in *from)
{
    int i;
    for (i = 0; i < s_nlpeers; i++) {
        lpeer_t *p = &s_lpeers[i];
        if (p->state != LP_PENDING && p->state != LP_LIVE)
            continue;
        if (p->endpoint.sin_addr.s_addr != from->sin_addr.s_addr || p->endpoint.sin_port != from->sin_port)
            continue;
        if (memcmp(p->nonce, buf + 5, 16))
            continue;
        if (p->state == LP_PENDING) {
            p->state = LP_LIVE;
            p->last_frame = GetTickCount64();
            s_lpending--;
            TRACE("punch complete: peer %s:%u is live\n", inet_ntoa(from->sin_addr), ntohs(from->sin_port));
        }
        if (type == 'P')
            punch_answer_send(p);
        return;
    }
    /* a punch/answer from an unknown peer: dropped (it may be our own bouncing back) */
}

/* remove a peer from the table (by index); called under s_lock */
static void lpeer_remove(int i)
{
    lpeer_t *p = &s_lpeers[i];
    if (p->state == LP_PENDING)
        s_lpending--;
    memmove(&s_lpeers[i], &s_lpeers[i + 1], (size_t)(s_nlpeers - i - 1) * sizeof(lpeer_t));
    s_nlpeers--;
}

/* add a pending peer (called by the worker under s_lock, no I/O); returns 0 on cap */
int fci_nic_list_add_peer(const char *endpoint, const uint8_t mac[6], const uint8_t nonce[16])
{
    struct addrinfo hints, *ai = NULL;
    char host[128], port[16];
    const char *colon;
    ULONGLONG now;
    if (s_nlpeers >= LIST_PEER_MAX)
        return 0;
    colon = strrchr(endpoint, ':');
    if (!colon || (size_t)(colon - endpoint) >= sizeof host || strlen(colon + 1) >= sizeof port)
        return 0;
    memcpy(host, endpoint, (size_t)(colon - endpoint));
    host[colon - endpoint] = 0;
    strcpy(port, colon + 1);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &ai) || !ai)
        return 0;
    if (s_lpending >= LIST_PENDING_MAX) {
        /* the pending cap: drop the oldest pending peer */
        int oldest = -1;
        int i;
        for (i = 0; i < s_nlpeers; i++)
            if (s_lpeers[i].state == LP_PENDING) {
                oldest = i;
                break;
            }
        if (oldest >= 0) {
            fprintf(stderr, "[FCI] network: pending peer cap (%d) reached, dropping oldest\n",
                    LIST_PENDING_MAX);
            fflush(stderr);
            lpeer_remove(oldest);
        }
    }
    {
        lpeer_t *p = &s_lpeers[s_nlpeers]; /* after any removal: the table has shifted */
        memcpy(&p->endpoint, ai->ai_addr, sizeof(struct sockaddr_in));
        freeaddrinfo(ai);
        memcpy(p->mac, mac, 6);
        memcpy(p->nonce, nonce, 16);
        p->state = LP_PENDING;
        now = GetTickCount64();
        p->next_punch = now;
        p->punch_end = now + PUNCH_WINDOW_MS;
        p->last_frame = now;
        s_lpending++;
        s_nlpeers++;
        return 1;
    }
}

/* the card's public endpoint as "ip:port" (for the worker's register call); 0 if not yet known */
int fci_nic_list_endpoint(char *buf, int buflen)
{
    if (!s_stun_known)
        return 0;
    snprintf(buf, (size_t)buflen, "%s:%u", inet_ntoa(s_stun_ep.sin_addr), ntohs(s_stun_ep.sin_port));
    return 1;
}

/* 1 when a peer with the given endpoint ("ip:port") is pending or live; called under s_lock */
int fci_nic_list_has_peer(const char *endpoint)
{
    struct addrinfo hints, *ai = NULL;
    char host[128], port[16];
    const char *colon;
    int i, have = 0;
    colon = strrchr(endpoint, ':');
    if (!colon || (size_t)(colon - endpoint) >= sizeof host || strlen(colon + 1) >= sizeof port)
        return 0;
    memcpy(host, endpoint, (size_t)(colon - endpoint));
    host[colon - endpoint] = 0;
    strcpy(port, colon + 1);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &ai) || !ai)
        return 0;
    for (i = 0; i < s_nlpeers; i++)
        if (memcmp(&s_lpeers[i].endpoint, ai->ai_addr, sizeof(struct sockaddr_in)) == 0) {
            have = 1;
            break;
        }
    freeaddrinfo(ai);
    return have;
}

/* the relay fallback (RECOMP_FCI_NET_RELAY): the hub keys a member by address, so one socket per
 * relay room (cap RELAY_MAX); the room is the nonce in hex, so it holds exactly the two cards. */
static void relay_hello(int i)
{
    char b[5 + 6 + COOKIE + 32];
    uint8_t mac[6];
    size_t n = strlen(s_relroom[i]);
    fci_nic_mac(mac);
    memcpy(b, MAGIC, 4); /* the hub's header, as send_datagram writes it */
    b[4] = 'H';
    memcpy(b + 5, mac, 6);
    memcpy(b + 5 + 6, s_relcookie[i], COOKIE);
    memcpy(b + 5 + 6 + COOKIE, s_relroom[i], n);
    send(s_relsock[i], b, (int)(5 + 6 + COOKIE + n), 0);
}

/* a hub socket for a relay room, opened on the first relay peer. Returns the socket index, -2 on a
 * socket error (the peer stays pending and keeps punching) or -1 on the cap. */
static int relay_socket_new(const lpeer_t *p)
{
    SOCKET s;
    u_long nb = 1;
    int i, n;
    if (!s_relay_hub_valid)
        return -2;
    for (i = 0; i < RELAY_MAX; i++)
        if (s_relsock[i] == INVALID_SOCKET)
            break;
    if (i == RELAY_MAX)
        return -1;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET || connect(s, (const struct sockaddr *)&s_relay_hub, sizeof s_relay_hub)
        || ioctlsocket(s, FIONBIO, &nb)) {
        int e = WSAGetLastError();
        if (s != INVALID_SOCKET)
            closesocket(s);
        fprintf(stderr, "[FCI] network: relay hub %s: no socket (%d), the peer keeps punching\n",
                s_relay_spec, e);
        fflush(stderr);
        return -2;
    }
    s_relsock[i] = s;
    memset(s_relcookie[i], 0, COOKIE);
    for (n = 0; n < 16; n++) /* the room: the peer's nonce in hex, 32 chars */
        sprintf(s_relroom[i] + n * 2, "%02x", p->nonce[n]);
    s_relroom[i][32] = 0;
    s_relhello[i] = GetTickCount64(); /* hello now: the cookie exchange starts */
    relay_hello(i);
    return i;
}

/* the relay room's nonce as 32 hex (the room name, for the log lines) */
static const char *relay_room_hex(const lpeer_t *p)
{
    static char hex[33];
    int n;
    for (n = 0; n < 16; n++)
        sprintf(hex + n * 2, "%02x", p->nonce[n]);
    hex[32] = 0;
    return hex;
}

static void receive_relays(void)
{
    int i, k;
    for (i = 0; i < RELAY_MAX; i++) {
        char buf[FRAME_MAX + 8];
        if (s_relsock[i] == INVALID_SOCKET)
            continue;
        for (k = 0; k < RX_QUEUE; k++) {
            struct sockaddr_in from;
            int fromlen = sizeof from;
            int n = recvfrom(s_relsock[i], buf, sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
            if (n == SOCKET_ERROR) {
                int e = WSAGetLastError(); /* WSAEWOULDBLOCK: no more this millisecond */
                if (e == WSAEMSGSIZE || e == WSAECONNRESET)
                    continue;
                break;
            }
            /* the socket is connected to the hub: only the hub's datagrams arrive */
            if (n < 5 || memcmp(buf, MAGIC, 4) || fromlen != sizeof from
                || from.sin_addr.s_addr != s_relay_hub.sin_addr.s_addr
                || from.sin_port != s_relay_hub.sin_port)
                continue;
            if (buf[4] == 'C' && n == 5 + COOKIE) {
                memcpy(s_relcookie[i], buf + 5, COOKIE);
                relay_hello(i); /* say hello with the cookie, as in hub mode */
                continue;
            }
            if (buf[4] != 'F' || n < 5 + 14 || n > 5 + ETHER_MAX)
                continue;
            {
                /* only the room's peer, with its pre-seeded source MAC; anything else is dropped */
                int j, ok = 0;
                for (j = 0; j < s_nlpeers; j++)
                    if (s_lpeers[j].state == LP_RELAY && s_lpeers[j].relay == i) {
                        const uint8_t *src = (const uint8_t *)buf + 5 + 6;
                        ok = !(src[0] & 1) && !memcmp(s_lpeers[j].mac, src, 6);
                        if (ok)
                            s_lpeers[j].last_frame = GetTickCount64();
                        break;
                    }
                if (!ok) {
                    stray(&from);
                    continue;
                }
            }
            if (s_rxq_count == RX_QUEUE) {
                s_rxq_head = (s_rxq_head + 1) % RX_QUEUE;
                s_rxq_count--;
            }
            {
                int slot = (s_rxq_head + s_rxq_count) % RX_QUEUE;
                memcpy(s_rxq[slot], buf + 5, (size_t)(n - 5));
                s_rxq_len[slot] = n - 5;
                s_rxq_count++;
            }
        }
    }
}

/* --- end LIST mode --- */

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
            if (route_frame(frame, len) && !s_sent++)
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
        if (s_list) {
            /* LIST mode: classify STUN, punch, frame */
            if (n >= 20 && (uint8_t)buf[4] == 0x21 && (uint8_t)buf[5] == 0x12 && (uint8_t)buf[6] == 0xA4
                && (uint8_t)buf[7] == 0x42) {
                if (fromlen == sizeof from)
                    stun_recv(buf, n, &from);
                continue;
            }
            if (n < 5 || memcmp(buf, MAGIC, 4))
                continue;
            if ((buf[4] == 'P' || buf[4] == 'A') && n == 21) {
                if (fromlen == sizeof from)
                    punch_recv(buf, buf[4], &from);
                continue;
            }
            if (buf[4] != 'F' || n < 5 + 14 || n > 5 + ETHER_MAX)
                continue;
            /* a frame: only from a live peer, with that peer's MAC */
            {
                int i;
                lpeer_t *lp = NULL;
                for (i = 0; i < s_nlpeers; i++)
                    if (s_lpeers[i].state == LP_LIVE
                        && s_lpeers[i].endpoint.sin_addr.s_addr == from.sin_addr.s_addr
                        && s_lpeers[i].endpoint.sin_port == from.sin_port) {
                        lp = &s_lpeers[i];
                        break;
                    }
                if (!lp) {
                    stray(&from);
                    continue;
                }
                {
                    const uint8_t *src = (const uint8_t *)buf + 5 + 6;
                    if (src[0] & 1) { /* no card sends from a group address */
                        stray(&from);
                        continue;
                    }
                    if (memcmp(lp->mac, src, 6)) {
                        stray(&from);
                        continue;
                    }
                    lp->last_frame = GetTickCount64();
                }
            }
        } else if (n < 5 || memcmp(buf, MAGIC, 4)) {
            continue;
        } else if (s_direct) { /* only peers reach the guest, each with its own card's frames */
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

/* LIST mode: the punch schedule, STUN refresh, and peer expiries (called under s_lock in the tick) */
static void list_tick(ULONGLONG now)
{
    int i;
    /* STUN: at link-up and every 60 s (skipped when _ENDPOINT set s_stun_known at init) */
    if (s_stun_srv_valid && now >= s_stun_next) {
        stun_send();
        s_stun_next = now + STUN_PERIOD_MS;
    }
    /* relay hellos: the hub leaves a quiet member, so the room's socket keeps saying hello */
    for (i = 0; i < RELAY_MAX; i++)
        if (s_relsock[i] != INVALID_SOCKET && now >= s_relhello[i]) {
            relay_hello(i);
            s_relhello[i] = now + HELLO_PERIOD_MS;
        }
    /* punch schedule and expiries */
    for (i = 0; i < s_nlpeers; i++) {
        lpeer_t *p = &s_lpeers[i];
        if (p->state == LP_PENDING) {
            if (now >= p->punch_end) {
                /* 3 s with no answer: the relay fallback, if set, keeps the peer in its nonce room */
                if (s_relay_spec[0]) {
                    int r = relay_socket_new(p);
                    if (r == -1) {
                        fprintf(stderr,
                                "[FCI] network: relay socket cap (%d) reached, "
                                "dropping peer %s:%u\n",
                                RELAY_MAX, inet_ntoa(p->endpoint.sin_addr), ntohs(p->endpoint.sin_port));
                        fflush(stderr);
                        lpeer_remove(i);
                        i--;
                        continue;
                    }
                    if (r == -2) {
                        /* no socket: stay pending, keep punching, and try again at the next window */
                        p->punch_end = now + PUNCH_WINDOW_MS;
                        p->next_punch = now;
                        continue;
                    }
                    p->state = LP_RELAY;
                    p->relay = r;
                    s_lpending--;
                    p->last_frame = now;
                    fprintf(stderr, "[FCI] network: punch to %s:%u failed, using relay room %.8s\n",
                            inet_ntoa(p->endpoint.sin_addr), ntohs(p->endpoint.sin_port), relay_room_hex(p));
                    fflush(stderr);
                    continue;
                }
                fprintf(stderr, "[FCI] network: punch to %s:%u failed, no relay set: peer dropped\n",
                        inet_ntoa(p->endpoint.sin_addr), ntohs(p->endpoint.sin_port));
                fflush(stderr);
                lpeer_remove(i);
                i--;
                continue;
            }
            if (now >= p->next_punch) {
                punch_send(p);
                p->next_punch += PUNCH_PERIOD_MS;
            }
        } else if (now - p->last_frame > PEER_EXPIRE_MS) {
            /* LP_LIVE and LP_RELAY: expire after 15 s without a frame */
            if (p->state == LP_RELAY) {
                fprintf(stderr, "[FCI] network: relay room %.8s: no frame in 15 s, peer expired\n",
                        relay_room_hex(p));
                relay_socket_close(p->relay);
                p->relay = -1;
            } else {
                fprintf(stderr, "[FCI] network: peer %s:%u: no frame in 15 s, expired\n",
                        inet_ntoa(p->endpoint.sin_addr), ntohs(p->endpoint.sin_port));
            }
            fflush(stderr);
            lpeer_remove(i);
            i--;
        }
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
        if (!s_direct && !s_list && now >= next_hello) {
            hello();
            next_hello = now + 2000;
        }
        if (s_list) {
            list_tick(now);
            receive_relays(); /* before receive(): the relay queue shares s_rxq */
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

/* RECOMP_FCI_NET_LIST=<url>: the internet host list. The card socket is unbound (ephemeral port) and
 * unconnected: it sends STUN and punches to any address, and receives from live peers. _ENDPOINT skips
 * STUN (local test); _RELAY names a hub for the relay fallback (a TODO this round).
 * 0 on a socket error, with the failure in err. */
static int connect_list(const char *url, char *err, int errlen)
{
    WSADATA wsa;
    u_long nb = 1;
    const char *endpoint = getenv("RECOMP_FCI_NET_ENDPOINT"), *relay = getenv("RECOMP_FCI_NET_RELAY");
    if (strlen(url) >= sizeof s_list_url) {
        snprintf(err, errlen, "URL too long");
        return 0;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        snprintf(err, errlen, "WSAStartup failed (%d)", WSAGetLastError());
        return 0;
    }
    strcpy(s_list_url, url);
    s_relay_spec[0] = 0;
    for (int i = 0; i < RELAY_MAX; i++) /* static storage starts at 0, which is a valid SOCKET */
        s_relsock[i] = INVALID_SOCKET;
    s_relay_hub_valid = 0;
    if (relay && *relay) {
        /* the relay hub (host:port, no room): resolve it now so a bad value is seen at link-up */
        char host[256], port[16];
        const char *colon = strrchr(relay, ':');
        struct addrinfo hints, *ai = NULL;
        if (!colon || (size_t)(colon - relay) >= sizeof host || strlen(colon + 1) >= sizeof port) {
            snprintf(err, errlen, "relay %s is not <host:port>", relay);
            return 0;
        }
        snprintf(s_relay_spec, sizeof s_relay_spec, "%s", relay);
        memcpy(host, relay, (size_t)(colon - relay));
        host[colon - relay] = 0;
        strcpy(port, colon + 1);
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, port, &hints, &ai) || !ai) {
            snprintf(err, errlen, "relay %s does not resolve", relay);
            return 0;
        }
        memcpy(&s_relay_hub, ai->ai_addr, sizeof s_relay_hub);
        s_relay_hub_valid = 1;
        freeaddrinfo(ai);
        fprintf(stderr, "[FCI] network: relay hub %s (fallback when a punch fails)\n", relay);
        fflush(stderr);
    }
    /* the card socket: bound to a local port, unconnected (punches and STUN go to any address) */
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) {
        snprintf(err, errlen, "no socket (%d)", WSAGetLastError());
        return 0;
    }
    if (ioctlsocket(s_sock, FIONBIO, &nb)) {
        int e = WSAGetLastError();
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        snprintf(err, errlen, "non-blocking mode failed (%d)", e);
        return 0;
    }
    s_list = 1;
    /* the endpoint: from _ENDPOINT (local test) or STUN (the real path) */
    if (endpoint && *endpoint) {
        char host[128], port[16];
        const char *colon = strrchr(endpoint, ':');
        if (colon && (size_t)(colon - endpoint) < sizeof host && strlen(colon + 1) < sizeof port) {
            memcpy(host, endpoint, (size_t)(colon - endpoint));
            host[colon - endpoint] = 0;
            strcpy(port, colon + 1);
            {
                struct addrinfo hints, *ai = NULL;
                struct sockaddr_in bind_addr;
                memset(&hints, 0, sizeof hints);
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_DGRAM;
                if (!getaddrinfo(host, port, &hints, &ai) && ai) {
                    memcpy(&s_stun_ep, ai->ai_addr, sizeof s_stun_ep);
                    s_stun_known = 1;
                    freeaddrinfo(ai);
                    /* bind the card socket to this port (INADDR_ANY) so punches use it */
                    memset(&bind_addr, 0, sizeof bind_addr);
                    bind_addr.sin_family = AF_INET;
                    bind_addr.sin_addr.s_addr = INADDR_ANY;
                    bind_addr.sin_port = s_stun_ep.sin_port;
                    if (bind(s_sock, (const struct sockaddr *)&bind_addr, sizeof bind_addr)) {
                        closesocket(s_sock);
                        s_sock = INVALID_SOCKET;
                        snprintf(err, errlen, "bind to %s failed (%d)", endpoint, WSAGetLastError());
                        return 0;
                    }
                    fprintf(stderr, "[FCI] network: endpoint from _ENDPOINT %s (bound)\n", endpoint);
                    fflush(stderr);
                } else {
                    closesocket(s_sock);
                    s_sock = INVALID_SOCKET;
                    snprintf(err, errlen, "endpoint %s not an address", endpoint);
                    return 0;
                }
            }
        } else {
            closesocket(s_sock);
            s_sock = INVALID_SOCKET;
            snprintf(err, errlen, "endpoint %s not <ip:port>", endpoint);
            return 0;
        }
    } else {
        /* bind port 0 (the OS picks) and log the local port */
        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof bind_addr);
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = 0;
        if (bind(s_sock, (const struct sockaddr *)&bind_addr, sizeof bind_addr)) {
            int e = WSAGetLastError();
            closesocket(s_sock);
            s_sock = INVALID_SOCKET;
            snprintf(err, errlen, "bind failed (%d)", e);
            return 0;
        }
        {
            struct sockaddr_in local;
            int llen = sizeof local;
            if (!getsockname(s_sock, (struct sockaddr *)&local, &llen)) {
                fprintf(stderr, "[FCI] network: card socket bound to port %u\n", ntohs(local.sin_port));
                fflush(stderr);
            }
        }
        /* resolve the STUN server (_STUN overrides, default stun.cloudflare.com:3478) */
        {
            const char *stun = getenv("RECOMP_FCI_NET_STUN");
            char shost[128], sport[16];
            const char *colon;
            if (stun && *stun) {
                colon = strrchr(stun, ':');
                if (!colon || (size_t)(colon - stun) >= sizeof shost || strlen(colon + 1) >= sizeof sport) {
                    fprintf(stderr, "[FCI] RECOMP_FCI_NET_STUN=%s: not <host:port>\n", stun);
                    return 0;
                }
                memcpy(shost, stun, (size_t)(colon - stun));
                shost[colon - stun] = 0;
                strcpy(sport, colon + 1);
            } else {
                strcpy(shost, "stun.cloudflare.com");
                strcpy(sport, "3478");
            }
            struct addrinfo hints, *ai = NULL;
            memset(&hints, 0, sizeof hints);
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            if (!getaddrinfo(shost, sport, &hints, &ai) && ai) {
                memcpy(&s_stun_srv, ai->ai_addr, sizeof s_stun_srv);
                s_stun_srv_valid = 1;
                freeaddrinfo(ai);
                s_stun_next = 0; /* send the first STUN at the next tick */
            } else {
                fprintf(stderr,
                        "[FCI] network: STUN server %s:%s didn't resolve: no public endpoint, "
                        "hosting and joining off\n",
                        shost, sport);
                fflush(stderr);
            }
        }
    }
    return 1;
}

/* returns 0 when the page cannot be trapped or RECOMP_FCI_NET(_PEERS|_LIST) cannot be used */
int fci_nic_init(ptrdiff_t mem_offset)
{
    DWORD old;
    const char *net = getenv("RECOMP_FCI_NET"), *peers = getenv("RECOMP_FCI_NET_PEERS");
    const char *list = getenv("RECOMP_FCI_NET_LIST");
    const char *trace = getenv("RECOMP_FCI_NET_TRACE");
    uint8_t m[6];
    int nset = (net && *net) + (peers && *peers) + (list && *list);
    s_trace = trace && *trace == '1';
    s_mem_offset = mem_offset;
    if (nset > 1) {
        fprintf(stderr, "[FCI] RECOMP_FCI_NET, _PEERS and _LIST: one link at a time\n");
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
    } else if (list && *list) {
        char err[128];
        if (!connect_list(list, err, sizeof err)) {
            fprintf(stderr, "[FCI] RECOMP_FCI_NET_LIST=%s: %s\n", list, err);
            return 0;
        }
        fci_nic_mac(m);
        fprintf(stderr, "[FCI] network: link up (host list %s), card %02X:%02X:%02X:%02X:%02X:%02X\n", list,
                m[0], m[1], m[2], m[3], m[4], m[5]);
        fflush(stderr);
        fci_net_worker_start(); /* LIST mode: the worker thread does the host-list HTTP */
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

/* the worker's probe flag: set by route_frame on broadcast, cleared by the worker after a fetch.
 * Returns 1 and clears the flag if a probe was pending. */
int fci_nic_worker_probe_take(void) { return InterlockedExchange(&s_probe, 0) != 0; }

/* the list URL (for the worker). Returns NULL if not in LIST mode. */
const char *fci_nic_list_url(void) { return s_list ? s_list_url : NULL; }

/* the NIC's lock (the worker takes it for fci_nic_list_* calls) */
SRWLOCK *fci_nic_lock(void) { return &s_lock; }

/* MCPX network card (guest FEF00000), the nForce MAC the game's XNET drives itself: registers, descriptor
 * rings and interrupt. Its Ethernet frames go over UDP to a hub (RECOMP_FCI_NET=<host>:<port>[/<room>],
 * tools/fci_hub.py), straight to peer cards (RECOMP_FCI_NET_PEERS), or through the internet host list
 * (RECOMP_FCI_NET_LIST, with hole punching and STUN); without any the link is down, as with no cable.
 * research/docs/multiplayer.md */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmio_decode.h"
#include "fci_image.h"
#include "fci_frame.h"
#include "fci_stun.h"
#include "fci_net.h"
#include "fci_net_worker.h"
#include "fci_turn.h"

extern uint32_t fci_xbe_version; /* the XBE certificate's Version (fci_main.c): the triple's version */

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
#define MAGIC  "FCN1"  /* datagrams: magic, kind ('H' hello: MAC, cookie, room (version-scoped) or knock:
                          * MAC, triple; 'C' cookie; 'F' frame; 'P'/'A' punch) */
#define COOKIE 8       /* the hub's proof that this address is ours (tools/fci_hub.py) */

/* LIST mode: the internet host list (RECOMP_FCI_NET_LIST) */
#define LIST_PEER_MAX    16
#define LIST_PENDING_MAX 8
#define PUNCH_PERIOD_MS  100
#define PUNCH_WINDOW_MS  3000
#define PEER_EXPIRE_MS   15000
#define TURN_OPEN_MS     1000 /* the host's datagram to a relayed guest: opens and keeps its NAT mapping */
#define STUN_PERIOD_MS   60000

typedef enum { LP_PENDING, LP_LIVE, LP_RELAY, LP_TURN_WAIT } lp_state;

typedef struct {
    struct sockaddr_in endpoint;
    struct sockaddr_in local_ep; /* the peer's LAN address (when both sides are behind the same NAT) */
    int has_local;               /* 1 when local_ep is valid (cleared once the peer is live) */
    uint8_t mac[6];
    uint8_t nonce[16];
    lp_state state;
    int relay;            /* the relay socket (s_relsock) when in the relay room; -1 otherwise */
    int turn_ch;          /* the TURN channel (fci_turn) when the guest relays to this peer; -1 else */
    int guest;            /* 1 on the guest side (we connected to this host): only a guest asks for TURN */
    int via_turn;         /* 1 when this peer's frames arrive over a TURN relay (the host side):
                              the source is the TURN server, so they are matched by MAC, not address */
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
    uint8_t mac[6]; /* the card at that endpoint, from its knock (direct) or admission (LAN) */
    int has_mac;
    ULONGLONG last_heard; /* the last frame from addr: the endpoint is alive while this is recent */
    ULONGLONG last_knock; /* the last knock from the card (its admission, LAN and direct) */
} peer_t;
static peer_t s_peers[PEERS_MAX];
static int s_npeers, s_direct; /* s_direct: frames go to s_peers, not to a hub */
static unsigned long s_unrouted, s_stray;
/* LAN mode (RECOMP_FCI_NET_LAN=1, fci.ini network-lan): the card's frames travel as plain UDP
 * broadcast on the players' own L2 segment (the subnet broadcast of each up IPv4 adapter), so
 * nothing leaves it: no hub, no list, no worker. The learned MAC table is s_peers (32 entries,
 * twice the game's 16-player cap, the entry shape already mac + endpoint). */
#define LAN_PORT_DEFAULT 47920 /* 3074 is taken by Windows' own Xbox services */
#define LAN_REFRESH_MS 30000   /* the adapter list: a 30 s timer and a send failure re-read it (Wi-Fi) */
static int s_lan;
static uint16_t s_lan_port;
static uint16_t s_lan_bcports[8]; /* broadcast destination ports; default: the bind port */
static int s_lan_nbcports;
static struct in_addr s_lan_bc[8]; /* the subnet broadcast of each up IPv4 adapter */
static int s_lan_nbc;
static int s_lan_bc_logged; /* the list was logged at start; a refresh logs only a change */
static ULONGLONG s_lan_bc_refresh; /* next adapter refresh (GetTickCount64) */
static ULONGLONG s_lan_knock_next, s_dir_knock_next; /* next knock (GetTickCount64) */
static void lan_adapters_refresh(void);
int fci_nic_start_list(const char *url); /* defined below; the lazy starter (above) calls it */
int fci_nic_start_lan(void);             /* defined below; the menu transport entry (above) calls it */
static char s_room[33]; /* the hub's room name, scoped by the triple (room_scope) */
static uint8_t s_cookie[COOKIE];
static uint8_t s_mac[6];
static int s_mac_made;
static uint8_t s_rxq[RX_QUEUE][FRAME_MAX];
static int s_rxq_len[RX_QUEUE], s_rxq_head, s_rxq_count;
static unsigned long s_sent, s_received, s_dropped;
/* traffic by path, for the operator's relay cost: datagram payload bytes (FCN1 'F' + frame) */
enum { TP_DIRECT, TP_TURN, TP_HUB, TP_N };
static unsigned long long s_tfc[TP_N][4]; /* tx frames, tx bytes, rx frames, rx bytes */
static unsigned long long s_tfc_logged;   /* the frame total at the last traffic line */
static ULONGLONG s_tfc_next;
#define TRAFFIC_LOG_MS 60000
static void traffic_tx(int path, uint32_t len) { s_tfc[path][0]++, s_tfc[path][1] += len + 5; }
static void traffic_rx(int path, int len) { s_tfc[path][2]++, s_tfc[path][3] += (unsigned)len + 5; }

/* the cumulative traffic line: once a minute while frames flow, and at shutdown (force) */
static void traffic_summary(ULONGLONG now, int force)
{
    static const char *const name[TP_N] = { "direct", "relay", "hub" };
    unsigned long long total = 0;
    int i, shown = 0;
    for (i = 0; i < TP_N; i++)
        total += s_tfc[i][0] + s_tfc[i][2];
    if (total == s_tfc_logged || (!force && now < s_tfc_next))
        return;
    s_tfc_next = now + TRAFFIC_LOG_MS;
    s_tfc_logged = total;
    fprintf(stderr, "[FCI] network: traffic:");
    for (i = 0; i < TP_N; i++) {
        if (!(s_tfc[i][0] + s_tfc[i][2]))
            continue;
        fprintf(stderr, "%s %s tx %llu frames %llu bytes, rx %llu frames %llu bytes", shown++ ? ";" : "", name[i],
                s_tfc[i][0], s_tfc[i][1], s_tfc[i][2], s_tfc[i][3]);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}
/* q24: the gate between the wire and the guest's RX queue. Every frame (card socket, relay rooms,
 * TURN relay) passes the sanity check (src/fci_frame.c) and the per-source-MAC rate cap before it
 * is enqueued; the envelope's drops (no FCN1 magic, an unknown kind, an out-of-size datagram) are
 * counted too. Drops are logged two ways: detailed lines (the first eight, then one a 10 s) and a
 * counters line ("gate drops: ...", every counter by name, cumulative, at most once a 10 s) that
 * tools/net_fuzz.py judges. research/qwen/notes/q24-harden.md */
#define RATE_MAX 64
typedef struct {
    uint8_t mac[6];
    int used;
    ULONGLONG t0;
    unsigned long n;
} rate_t;
static rate_t s_rate[RATE_MAX];
static unsigned long s_badframe[FCI_FRAME_NREASONS]; /* frames dropped by the sanity check, by reason */
static unsigned long s_baddgram;                     /* datagrams dropped by the envelope check */
static unsigned long s_ratelimit;                    /* frames dropped by the rate cap */
static unsigned long s_passed;                       /* frames the gate put in the RX queue */
static unsigned long s_gate_logged;                  /* detailed gate drop lines printed so far */
static ULONGLONG s_gate_next;                        /* the next detailed gate drop line (GetTickCount64) */
static unsigned long s_summary_total; /* the gate's total drop count at the last summary line */
static ULONGLONG s_summary_next;      /* the next gate summary line (GetTickCount64) */
static int s_trace
        = -1; /* RECOMP_FCI_NET_TRACE=1: control writes and interrupts (the first 400) and frames */
static unsigned s_traced;

/* LIST mode state */
static int s_list; /* 1 when RECOMP_FCI_NET_LIST is in use */
static lpeer_t s_lpeers[LIST_PEER_MAX];
static int s_nlpeers;
static int s_lpending;                /* count of LP_PENDING peers (cap at LIST_PENDING_MAX) */
static volatile LONG s_lpeer_dropped; /* count of peers dropped due to failure (no relay, punch failed) */
static struct sockaddr_in s_stun_ep;  /* the card's public endpoint (from STUN or _ENDPOINT) */
static int s_stun_known;              /* 1 when the endpoint is set */
static ULONGLONG s_stun_next;         /* next STUN send deadline */
static uint32_t s_stun_txn;           /* STUN transaction ID counter */
static struct sockaddr_in s_stun_srv; /* the STUN server address (resolved at init) */
static int s_stun_srv_valid;          /* 1 when the STUN server is resolved */
static char s_list_url[512];          /* the Worker base URL (for the worker thread, round B) */
/* menu mode (RECOMP_FCI_NET_MENU=1, fci.ini network-menu): the card reports link up at boot without
 * a transport, so the online items in the pre-game menu are enabled; the player's pick starts the
 * transport (fci_nic_start_menu_transport). s_up: link up without a socket (the menu, before a pick).
 * s_menu_list_url: the LIST URL for the menu path (RECOMP_FCI_NET_LIST, overridable by fci.ini). */
static int s_up;
static int s_menu;
static char s_menu_list_url[512];
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
static volatile LONG s_probe; /* A browser entry takes precedence over a discovery broadcast. */
/* the TURN relay (guest side): a failed punch moves the peer to LP_TURN_WAIT, the worker mints a
 * credential (POST /v1/turn) and starts the allocation; when it is ready the peer is relayed over
 * it and the worker posts the relayed connect (research/qwen/notes/q17-turn-design.md) */
static volatile LONG s_turn_need;  /* a peer is in LP_TURN_WAIT: the worker should mint a credential */
static volatile LONG s_turn_ready; /* the allocation is ready: the worker should post the relayed connect */
static int s_turn_ok;              /* RECOMP_FCI_NET_TURN != "0" */
static int s_punch_off;            /* RECOMP_FCI_NET_PUNCH == "0": a test switch, every pair goes to the relay */

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

/* LINK_UP while a transport is up (a socket) or while the card is up in menu mode (s_up, before a
 * pick): the online items in the pre-game menu are enabled on either. */
ULONG fci_nic_link_state(void) { return (s_sock != INVALID_SOCKET || s_up) ? LINK_UP : 0; }

/* 1 while a transport is up (a socket exists). The pacer reads it every frame (fci_xbr_glue.cpp), so
 * a transport started after boot (the menu, later) engages the 30-updates lock the moment it starts.
 * Menu mode (s_up) does not engage the lock: no transport, no game traffic. */
int fci_nic_transport_active(void) { return s_sock != INVALID_SOCKET; }

/* 1 while menu mode is on (RECOMP_FCI_NET_MENU=1): the gate (fci_lan_menu.c) redirects the online
 * picks and relabels the menu only in this mode. */
int fci_nic_menu_mode(void) { return s_menu; }

/* The lazy LIST starter: both the boot path (RECOMP_FCI_NET_LIST) and the menu path (the gate) call
 * it. Idempotent: a second start logs and leaves the first transport. With no URL: the Q4 log line,
 * no socket (s_up stays 1, the browser is empty). 1 when a socket is up. */
int fci_nic_start_list_lazy(const char *url)
{
    if (s_sock != INVALID_SOCKET) {
        fprintf(stderr, "[FCI] network: a transport is already up; the list is not started\n");
        fflush(stderr);
        return 0;
    }
    if (!url || !*url) {
        fprintf(stderr, "[FCI] network: INTERNET: no list URL set (set network-list); no transport started\n");
        fflush(stderr);
        return 0;
    }
    return fci_nic_start_list(url);
}

/* The menu path's transport entry (the gate, fci_lan_menu.c): mode 0 = LAN (SYSTEM LINK),
 * mode 1 = LIST (INTERNET). Idempotent: a second pick logs and leaves the first transport
 * (\"first choice wins until restart\"). 1 when a transport is up after this call. */
int fci_nic_start_menu_transport(int mode)
{
    const char *item = mode == 0 ? "LAN" : "INTERNET";
    if (s_sock != INVALID_SOCKET) {
        if (mode == 1 && s_list) {
            InterlockedExchange(&s_probe, FCI_LIST_PROBE_ENTRY);
            return 1;
        }
        fprintf(stderr, "[FCI] network: transport already up; ignoring %s\n", item);
        fflush(stderr);
        return 1;
    }
    if (mode == 0)
        return fci_nic_start_lan() ? 1 : 0;
    if (!fci_nic_start_list_lazy(s_menu_list_url))
        return 0;
    InterlockedExchange(&s_probe, FCI_LIST_PROBE_ENTRY);
    return 1;
}

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
    s_lan = 0;
    s_lan_nbc = s_lan_nbcports = 0;
    s_lan_bc_logged = 0;
    s_lan_knock_next = s_dir_knock_next = 0;
    s_up = 0;
    s_menu = 0;
    for (i = 0; i < RELAY_MAX; i++)
        relay_socket_close(i); /* the relay rooms leave with the process */
    fci_turn_stop();           /* the relay allocation leaves with the process */
    traffic_summary(0, 1);
    ReleaseSRWLockExclusive(&s_lock);
}

/* to the hub (to == NULL) or to one peer; a lost datagram is a lost frame, as on a wire */
static void send_datagram(char kind, const void *data, int len);

static void send_datagram_addr(const struct sockaddr_in *to, char kind, const void *data, int len)
{
    char buf[FRAME_MAX + 8];
    memcpy(buf, MAGIC, 4);
    buf[4] = kind;
    memcpy(buf + 5, data, (size_t)len);
    sendto(s_sock, buf, len + 5, 0, (const struct sockaddr *)to, sizeof *to);
}

/* with no peer: the connected socket (the hub). Not send_datagram, which comes back here */
static void send_datagram_to(const peer_t *to, char kind, const void *data, int len)
{
    if (to) {
        send_datagram_addr(&to->addr, kind, data, len);
    } else {
        char buf[FRAME_MAX + 8];
        memcpy(buf, MAGIC, 4);
        buf[4] = kind;
        memcpy(buf + 5, data, (size_t)len);
        send(s_sock, buf, len + 5, 0);
    }
}

static void send_datagram(char kind, const void *data, int len) { send_datagram_to(NULL, kind, data, len); }

/* a frame from the guest: unicast to the peer whose card it names, broadcast (DHCP included, as the hub
   relays it) to every peer; returns 1 if it went out, 0 if dropped */
/* a frame out over the relay: TURN channel (the guest's relay) or the hub's room socket (fallback) */
static void relay_out(const lpeer_t *p, const uint8_t *f, uint32_t len)
{
    if (p->turn_ch >= 0) {
        /* through TURN the payload is the card datagram (FCN1 'F' + frame): the host reads it
         * on its card socket from our relayed address, like a direct peer's */
        char buf[FRAME_MAX + 8];
        memcpy(buf, MAGIC, 4);
        buf[4] = 'F';
        memcpy(buf + 5, f, (size_t)len);
        fci_turn_send(p->turn_ch, buf, (int)len + 5);
        traffic_tx(TP_TURN, len);
    } else {
        relay_send_frame(p->relay, f, len);
        traffic_tx(TP_HUB, len);
    }
}

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
                    traffic_tx(p->via_turn ? TP_TURN : TP_DIRECT, len);
                    sent = 1;
                } else if (p->state == LP_RELAY) {
                    relay_out(p, f, len);
                    sent = 1;
                }
            }
            InterlockedCompareExchange(&s_probe, FCI_LIST_PROBE_DISCOVERY, 0);
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
                traffic_tx(p->via_turn ? TP_TURN : TP_DIRECT, len);
            } else {
                relay_out(p, f, len);
            }
            return 1;
        }
        if (!s_unrouted++) /* XNET answers only cards it heard from, so this is unexpected but harmless */
            fprintf(stderr, "[FCI] network: a frame to a card no peer has shown, dropped\n"), fflush(stderr);
        return 0;
    }
    if (s_lan) {
        /* LAN mode: a broadcast goes to every adapter's subnet broadcast at every broadcast port
         * (the game's ~1/s discovery is the keepalive, so no hello); a unicast goes to the table */
        char buf[FRAME_MAX + 8];
        int j, failed = 0;
        memcpy(buf, MAGIC, 4);
        buf[4] = 'F';
        memcpy(buf + 5, f, (size_t)len);
        if (f[0] & 1) {
            static ULONGLONG next_log;
            ULONGLONG now = GetTickCount64();
            if (s_lan_nbc == 0)
                lan_adapters_refresh(); /* the list went empty: re-read it before giving up */
            for (i = 0; i < s_lan_nbc; i++)
                for (j = 0; j < s_lan_nbcports; j++) {
                    struct sockaddr_in to;
                    memset(&to, 0, sizeof to);
                    to.sin_family = AF_INET;
                    to.sin_addr = s_lan_bc[i];
                    to.sin_port = htons(s_lan_bcports[j]);
                    if (sendto(s_sock, buf, (int)len + 5, 0, (const struct sockaddr *)&to, sizeof to)
                        == SOCKET_ERROR)
                        failed = 1;
                }
            if (failed)
                lan_adapters_refresh(); /* an address gone (Wi-Fi changed): re-read the list once */
            if (now >= next_log && s_lan_nbc) {
                next_log = now + 5000; /* about one a second: the steady state stays quiet */
                fprintf(stderr, "[FCI] network: LAN broadcast %uB -> %s:%u (x%d adapters, %d ports)\n",
                        len, inet_ntoa(s_lan_bc[0]), s_lan_bcports[0], s_lan_nbc, s_lan_nbcports);
                fflush(stderr);
            }
            return s_lan_nbc > 0;
        }
        for (i = 0; i < PEERS_MAX; i++)
            if (s_peers[i].has_mac && !memcmp(s_peers[i].mac, f, 6)) {
                sendto(s_sock, buf, (int)len + 5, 0, (const struct sockaddr *)&s_peers[i].addr,
                       sizeof s_peers[i].addr);
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

/* the gate's log line (q24): the first eight drops in detail, then one line every 10 s, so a
 * flood cannot flood the log */
static void gate_log(const char *what, const char *reason, int n, const uint8_t *mac,
                     const struct sockaddr_in *from)
{
    ULONGLONG now = GetTickCount64();
    if (s_gate_logged >= 8 && now < s_gate_next)
        return;
    s_gate_next = now + 10000;
    s_gate_logged++;
    fprintf(stderr, "[FCI] network: %s: %s (%d bytes) from %02X:%02X:%02X:%02X:%02X:%02X %s:%u, dropped\n",
            what, reason, n, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], inet_ntoa(from->sin_addr),
            ntohs(from->sin_port));
    fflush(stderr);
}

/* the gate's counters line (q24): the detailed drop lines are rate-limited (the first eight,
 * then one a 10 s), so a flood hides most of the reasons; this line carries every counter by
 * name, cumulative, at most once a 10 s whenever any of them changed. tools/net_fuzz.py judges
 * a run from the last one: it waits past the 10 s window, causes one drop, and reads the line.
 * research/qwen/notes/q24-harden.md */
static void gate_summary(void)
{
    ULONGLONG now = GetTickCount64();
    unsigned long total = s_baddgram + s_ratelimit + s_stray + s_passed;
    int i;
    for (i = 0; i < FCI_FRAME_NREASONS; i++)
        total += s_badframe[i];
    if (total == s_summary_total || now < s_summary_next)
        return;
    s_summary_next = now + 10000;
    s_summary_total = total;
    unsigned long bad = 0;
    for (i = 1; i < FCI_FRAME_NREASONS; i++)
        bad += s_badframe[i];
    fprintf(stderr, "[FCI] network: gate drops: envelope %lu, rate cap %lu, bad frame %lu (", s_baddgram,
            s_ratelimit, bad);
    for (i = 1; i < FCI_FRAME_NREASONS; i++)
        fprintf(stderr, "%s %lu%s", fci_frame_reason(i), s_badframe[i],
                i + 1 < FCI_FRAME_NREASONS ? ", " : "");
    fprintf(stderr, "), stranger %lu, passed %lu\n", s_stray, s_passed);
    fflush(stderr);
}

/* the envelope's drops: a datagram with no FCN1 magic, an unknown kind, or an out-of-size
 * payload (q24). Counted, logged rate-limited as the frame drops are. */
static void baddgram_log(const struct sockaddr_in *from, int n)
{
    s_baddgram++;
    gate_summary();
    ULONGLONG now = GetTickCount64();
    if (s_gate_logged >= 8 && now < s_gate_next)
        return;
    s_gate_next = now + 10000;
    s_gate_logged++;
    fprintf(stderr, "[FCI] network: a datagram with no FCN1 frame (%d bytes) from %s:%u, dropped\n", n,
            inet_ntoa(from->sin_addr), ntohs(from->sin_port));
    fflush(stderr);
}

/* an oversized datagram (q24): recvfrom fails WSAEMSGSIZE before the bytes are read, so there is
 * no source address to log; it is the envelope's out-of-size rule, counted as an envelope drop.
 * research/qwen/notes/q24-harden.md */
static void oversize_drop(void)
{
    s_baddgram++;
    gate_summary();
    ULONGLONG now = GetTickCount64();
    if (s_gate_logged >= 8 && now < s_gate_next)
        return;
    s_gate_next = now + 10000;
    s_gate_logged++;
    fprintf(stderr, "[FCI] network: an oversized datagram (WSAEMSGSIZE), dropped\n");
    fflush(stderr);
}

/* the per-source-MAC rate cap (q24): a fixed 1 s window of RATE_CAP frames. 200 is well above
 * the game's traffic (the recorded run averages a few frames a second per card; 60 fps game
 * frames are the upper bound) and bounds a single spoofed MAC at 200 x 1514 bytes/s. A slot quiet
 * for 10 s is reused; with 64 busy MACs a new one is uncapped, bounded by the receive budget and
 * the queue's overflow drop. */
#define RATE_CAP 200
#define RATE_MS  1000
static int rate_ok(const uint8_t *mac)
{
    ULONGLONG now = GetTickCount64();
    int i, free = -1;
    for (i = 0; i < RATE_MAX; i++) {
        if (s_rate[i].used && !memcmp(s_rate[i].mac, mac, 6))
            break;
        if ((!s_rate[i].used || now - s_rate[i].t0 >= 10 * RATE_MS) && free < 0)
            free = i;
    }
    if (i == RATE_MAX) { /* a new MAC takes a free or 10 s quiet slot; only 64 busy MACs leave it uncapped */
        if (free < 0)
            return 1;
        i = free;
    }
    if (!s_rate[i].used || memcmp(s_rate[i].mac, mac, 6))
        memcpy(s_rate[i].mac, mac, 6), s_rate[i].used = 1, s_rate[i].t0 = 0, s_rate[i].n = 0;
    if (now - s_rate[i].t0 >= RATE_MS)
        s_rate[i].t0 = now, s_rate[i].n = 0;
    if (++s_rate[i].n > RATE_CAP)
        return 0;
    return 1;
}

/* the gate between the wire and the guest's RX queue (q24): the frame sanity check
 * (src/fci_frame.c), the per-source-MAC rate cap, then the queue (the oldest goes when it is
 * full, as a full FIFO). 1 when enqueued, 0 when dropped (counted, logged rate-limited). */
static int rx_enqueue(const uint8_t *f, int n, const struct sockaddr_in *from)
{
    int r = fci_frame_sane(f, n);
    if (r != FCI_FRAME_OK) {
        s_badframe[r]++;
        gate_summary();
        gate_log("bad frame", fci_frame_reason(r), n, f + 6, from);
        return 0;
    }
    if (!rate_ok(f + 6)) {
        s_ratelimit++;
        gate_summary();
        gate_log("rate cap", "too many frames", n, f + 6, from);
        return 0;
    }
    s_passed++;
    if (s_rxq_count == RX_QUEUE) { /* the driver is not keeping up: the oldest goes, as a full FIFO */
        s_rxq_head = (s_rxq_head + 1) % RX_QUEUE;
        s_rxq_count--;
    }
    {
        int slot = (s_rxq_head + s_rxq_count) % RX_QUEUE;
        memcpy(s_rxq[slot], f, (size_t)n);
        s_rxq_len[slot] = n;
        s_rxq_count++;
    }
    return 1;
}

#define LAN_KNOCK_MS        1000  /* the knock: every second, as the game's discovery broadcast */
#define LAN_KNOCK_EXPIRE_MS 5000  /* a card quiet for 5 s (four lost knocks) is no longer admitted */

static peer_t *lan_find(const uint8_t *mac)
{
    int i;
    for (i = 0; i < PEERS_MAX; i++)
        if (s_peers[i].has_mac && !memcmp(s_peers[i].mac, mac, 6))
            return &s_peers[i];
    return NULL;
}

/* LAN mode: the knock admits a card's MAC (its triple is checked by the caller) and records its
 * endpoint, de-duplicated by MAC (a card reachable on two adapters is one entry). NULL when the
 * table is full. */
static peer_t *lan_admit(const uint8_t *mac, const struct sockaddr_in *from)
{
    ULONGLONG now = GetTickCount64();
    int i, free = -1;
    peer_t *p = lan_find(mac);
    if (p) {
        p->last_knock = now;
        if (p->addr.sin_addr.s_addr != from->sin_addr.s_addr || p->addr.sin_port != from->sin_port) {
            p->addr = *from;
            p->last_heard = now; /* the knock comes from the card's live adapter */
        }
        return p;
    }
    for (i = 0; i < PEERS_MAX; i++)
        if (!s_peers[i].has_mac && free < 0)
            free = i;
    if (free < 0)
        return NULL;
    s_peers[free].addr = *from;
    memcpy(s_peers[free].mac, mac, 6);
    s_peers[free].has_mac = 1;
    s_peers[free].last_heard = now;
    s_peers[free].last_knock = now;
    fprintf(stderr, "[FCI] network: LAN admit %02X:%02X:%02X:%02X:%02X:%02X <- %s:%u\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], inet_ntoa(from->sin_addr), ntohs(from->sin_port));
    fflush(stderr);
    return &s_peers[free];
}

/* a frame from an admitted MAC: the endpoint is kept while it is alive; it moves to a new source
 * only after 3 s of silence from the current one (an adapter change). NULL when the MAC is not
 * admitted, or its knocks have stopped (the card left): the frame is dropped. */
static peer_t *lan_frame_seen(const uint8_t *mac, const struct sockaddr_in *from)
{
    ULONGLONG now = GetTickCount64();
    peer_t *p = lan_find(mac);
    if (!p || now - p->last_knock >= LAN_KNOCK_EXPIRE_MS)
        return NULL;
    if (p->addr.sin_addr.s_addr == from->sin_addr.s_addr && p->addr.sin_port == from->sin_port)
        p->last_heard = now; /* the endpoint is alive: keep it */
    else if (now - p->last_heard >= 3000) {
        /* quiet for 3 s (an adapter change): the card moved, the new source wins */
        p->addr = *from;
        p->last_heard = now;
        fprintf(stderr, "[FCI] network: LAN learn %02X:%02X:%02X:%02X:%02X:%02X <- %s:%u (moved)\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], inet_ntoa(from->sin_addr),
                ntohs(from->sin_port));
        fflush(stderr);
    } /* a frame from another adapter while the endpoint is alive: keep it, no log */
    return p;
}

/* the compatibility triple every hello and knock carries (the one rule for every transport):
 * the title (0 = Instincts, 1 = Evolution), the disc's version (the XBE certificate's Version),
 * and the net protocol (fci_net.h). A peer is admitted only when all three match ours; a mismatch
 * is logged once per peer (10 s) and the peer is never admitted. research/docs/multiplayer.md */
#define NET_TRIPLE_LEN 6
static uint8_t s_triple[NET_TRIPLE_LEN];
static int s_triple_made;
static void net_triple_fill(uint8_t *b)
{
    if (!s_triple_made) {
        s_triple[0] = strstr(FCI_TITLE_NAME, "Evolution") ? 1 : 0;
        s_triple[1] = (uint8_t)fci_xbe_version;
        s_triple[2] = (uint8_t)(fci_xbe_version >> 8);
        s_triple[3] = (uint8_t)(fci_xbe_version >> 16);
        s_triple[4] = (uint8_t)(fci_xbe_version >> 24);
        s_triple[5] = FCI_NET_PROTO;
        s_triple_made = 1;
    }
    memcpy(b, s_triple, NET_TRIPLE_LEN);
}

/* ours is built on first use: a peer's knock can arrive before we send one */
static int net_triple_ok(const uint8_t *b)
{
    uint8_t ours[NET_TRIPLE_LEN];
    net_triple_fill(ours);
    return memcmp(b, ours, NET_TRIPLE_LEN) == 0;
}

/* one rate-limited line per peer (10 s): both sides' title, version and net protocol */
static void net_triple_mismatch(const uint8_t *mac, const uint8_t *b)
{
    static uint8_t seen[16][6];
    static ULONGLONG next_log[16];
    static int nseen;
    ULONGLONG now = GetTickCount64();
    uint32_t v = (uint32_t)b[1] | ((uint32_t)b[2] << 8) | ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 24);
    int i, free = -1;
    for (i = 0; i < nseen; i++) {
        if (!memcmp(seen[i], mac, 6))
            break;
        if (!seen[i][0] && free < 0)
            free = i;
    }
    if (i == nseen) {
        if (free < 0)
            free = nseen < 16 ? nseen : -1;
        if (free < 0)
            return; /* the table is full: a 17th mismatching peer is dropped without a log */
        memcpy(seen[free], mac, 6);
        i = free;
        if (free == nseen)
            nseen++;
    }
    if (now < next_log[i])
        return;
    next_log[i] = now + 10000;
    fprintf(stderr,
            "[FCI] network: peer %02X:%02X:%02X:%02X:%02X:%02X is %s version %u net protocol %u; "
            "we are %s version %u net protocol %u; not admitted\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], b[0] ? "evolution" : "instincts", (unsigned)v,
            (unsigned)b[5], strstr(FCI_TITLE_NAME, "Evolution") ? "evolution" : "instincts",
            (unsigned)fci_xbe_version, (unsigned)FCI_NET_PROTO);
    fflush(stderr);
}

/* the room name the card sends to the hub: "<room>~<the triple in hex>" (12 hex chars). The hub
 * keys its rooms by the name and never relays hellos, so a mismatched build lands in a different
 * room and never sees this one: a mismatch in hub mode shows as an empty room, with no line. The
 * user's room is cut to fit the name (and the cut is logged). */
static void room_scope(char *room, size_t roomlen)
{
    char hex[NET_TRIPLE_LEN * 2 + 1]; /* + the NUL sprintf writes */
    uint8_t t[NET_TRIPLE_LEN];
    size_t keep = roomlen - 1 - 1 - NET_TRIPLE_LEN * 2, n = strlen(room);
    int i;
    net_triple_fill(t); /* the triple is built on first use: not yet at hub start */
    for (i = 0; i < NET_TRIPLE_LEN; i++)
        sprintf(hex + i * 2, "%02x", t[i]);
    if (n > keep) {
        n = keep;
        fprintf(stderr, "[FCI] network: room name cut to %u chars to fit the version scope\n", (unsigned)n);
        fflush(stderr);
    }
    memcpy(room + n, "~", 1);
    memcpy(room + n + 1, hex, NET_TRIPLE_LEN * 2);
    room[n + 1 + NET_TRIPLE_LEN * 2] = 0;
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

/* the knock (LAN and direct peers): "FCN1" 'H' + our MAC + the triple, 17 bytes. A peer admits us
 * on it, as we admit it; the triple is what makes the two builds a match */
static void knock_send(const struct sockaddr_in *to)
{
    char b[6 + NET_TRIPLE_LEN];
    memcpy(b, s_mac, 6);
    net_triple_fill((uint8_t *)b + 6);
    send_datagram_addr(to, 'H', b, (int)sizeof b);
}

static void lan_knock(void)
{
    int i, j;
    for (i = 0; i < s_lan_nbc; i++)
        for (j = 0; j < s_lan_nbcports; j++) {
            struct sockaddr_in to;
            memset(&to, 0, sizeof to);
            to.sin_family = AF_INET;
            to.sin_addr = s_lan_bc[i];
            to.sin_port = htons(s_lan_bcports[j]);
            knock_send(&to);
        }
}

static void direct_knock(void)
{
    int i;
    for (i = 0; i < s_npeers; i++)
        knock_send(&s_peers[i].addr);
}

/* --- LIST mode: STUN, punch, peer table --- */

/* send a STUN-5 binding request from the card socket; the response's XOR-MAPPED-ADDRESS is the
 * card's public endpoint (the NAT mapping the punches will open) */
static void stun_send(void)
{
    uint8_t buf[20];
    if (!s_stun_srv_valid)
        return;
    s_stun_txn++;
    fci_stun_build_request(buf, s_stun_txn);
    sendto(s_sock, buf, 20, 0, (const struct sockaddr *)&s_stun_srv, sizeof s_stun_srv);
}

/* a STUN binding response (type 0x0101): extract the mapped address. Accept only from the
 * STUN server and only with our last transaction ID. Walk the attributes: prefer
 * XOR-MAPPED-ADDRESS (0x0020), fall back to MAPPED-ADDRESS (0x0001). */
static int stun_recv(const char *buf, int n, const struct sockaddr_in *from)
{
    uint8_t ip[4];
    uint16_t port;
    int xor, r;
    /* only from the STUN server we sent to */
    if (from->sin_addr.s_addr != s_stun_srv.sin_addr.s_addr || from->sin_port != s_stun_srv.sin_port)
        return 0;
    r = fci_stun_parse_response((const uint8_t *)buf, n, s_stun_txn, ip, &port, &xor);
    if (r != FCI_STUN_OK)
        return 0;
    s_stun_ep.sin_family = AF_INET;
    s_stun_ep.sin_port = htons(port);
    memcpy(&s_stun_ep.sin_addr.s_addr, ip, 4); /* wire order, as sin_addr holds it */
    if (!s_stun_known) {
        s_stun_known = 1;
        if (xor)
            fprintf(stderr, "[FCI] network: STUN endpoint %s:%u\n", inet_ntoa(s_stun_ep.sin_addr),
                    ntohs(s_stun_ep.sin_port));
        else
            fprintf(stderr, "[FCI] network: STUN endpoint (MAPPED) %s:%u\n",
                    inet_ntoa(s_stun_ep.sin_addr), ntohs(s_stun_ep.sin_port));
        fflush(stderr);
    }
    return 1;
}

/* send a punch datagram (FCN1 'P' + 16-byte nonce) to a peer's endpoint (and its LAN address,
 * when the Worker forwarded one: both sides are behind the same NAT, so the local path is the
 * one that will work) */
static void punch_send(const lpeer_t *p)
{
    char buf[21];
    if (s_punch_off && !p->via_turn) /* the host's opener to a relayed guest still goes */
        return;
    memcpy(buf, MAGIC, 4);
    buf[4] = 'P';
    memcpy(buf + 5, p->nonce, 16);
    sendto(s_sock, buf, 21, 0, (const struct sockaddr *)&p->endpoint, sizeof p->endpoint);
    if (p->has_local)
        sendto(s_sock, buf, 21, 0, (const struct sockaddr *)&p->local_ep, sizeof p->local_ep);
}

static void punch_answer_send(const lpeer_t *p)
{
    char buf[21];
    if (s_punch_off)
        return;
    memcpy(buf, MAGIC, 4);
    buf[4] = 'A';
    memcpy(buf + 5, p->nonce, 16);
    sendto(s_sock, buf, 21, 0, (const struct sockaddr *)&p->endpoint, sizeof p->endpoint);
}

/* a received punch 'P' or answer 'A' (FCN1 + type + 16-byte nonce): a 'P' is answered with an 'A';
 * an 'A' is never answered. Either one, with a matching nonce and endpoint (or LAN address),
 * moves a pending peer to live. When the local address wins, the peer's endpoint is switched to it. */
static void punch_recv(const char *buf, char type, const struct sockaddr_in *from)
{
    int i;
    if (s_punch_off)
        return;
    for (i = 0; i < s_nlpeers; i++) {
        lpeer_t *p = &s_lpeers[i];
        if (p->state != LP_PENDING && p->state != LP_LIVE)
            continue;
        /* match the primary endpoint or the LAN address (when both sides are behind the same NAT) */
        int is_local = 0;
        if (p->endpoint.sin_addr.s_addr == from->sin_addr.s_addr &&
            p->endpoint.sin_port == from->sin_port) {
            /* primary match */
        } else if (p->has_local &&
                   p->local_ep.sin_addr.s_addr == from->sin_addr.s_addr &&
                   p->local_ep.sin_port == from->sin_port) {
            is_local = 1;
        } else {
            continue;
        }
        if (memcmp(p->nonce, buf + 5, 16))
            continue;
        if (p->state == LP_PENDING) {
            p->state = LP_LIVE;
            p->last_frame = GetTickCount64();
            s_lpending--;
            s_lpeer_dropped = 0; /* a peer going live clears the dropped counter (Q29 r4) */
            if (is_local) {
                /* the local path won: the peer is live at its LAN address */
                memcpy(&p->endpoint, &p->local_ep, sizeof p->endpoint);
            }
            p->has_local = 0;
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

/* add a pending peer (called by the worker under s_lock, no I/O); returns 0 on cap.
 * `local` is the peer's LAN address ("ip:port") when the Worker forwarded one (both sides are
 * behind the same NAT); NULL or "" when there is none. The NIC punches both addresses and the
 * first valid answer wins. */
int fci_nic_list_add_peer(const char *endpoint, const char *local, const uint8_t mac[6],
                          const uint8_t nonce[16], int guest)
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
        p->guest = guest;
        p->relay = -1;
        p->turn_ch = -1;
        p->via_turn = 0;
        /* the peer's LAN address (when the Worker forwarded one) */
        p->has_local = 0;
        memset(&p->local_ep, 0, sizeof p->local_ep);
        if (local && local[0]) {
            char lhost[128], lport[16];
            const char *lcolon = strrchr(local, ':');
            if (lcolon && (size_t)(lcolon - local) < sizeof lhost && strlen(lcolon + 1) < sizeof lport) {
                memcpy(lhost, local, (size_t)(lcolon - local));
                lhost[lcolon - local] = 0;
                strcpy(lport, lcolon + 1);
                struct addrinfo lhints, *lai = NULL;
                memset(&lhints, 0, sizeof lhints);
                lhints.ai_family = AF_INET;
                lhints.ai_socktype = SOCK_DGRAM;
                if (!getaddrinfo(lhost, lport, &lhints, &lai) && lai) {
                    memcpy(&p->local_ep, lai->ai_addr, sizeof p->local_ep);
                    p->has_local = 1;
                    freeaddrinfo(lai);
                }
            }
        }
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

/* the card socket's bound port (for the worker's local-address detection); 0 if not in LIST mode */
int fci_nic_list_local_port(void)
{
    struct sockaddr_in a;
    int len = sizeof a;
    if (!s_list || s_sock == INVALID_SOCKET)
        return 0;
    if (getsockname(s_sock, (struct sockaddr *)&a, &len))
        return 0;
    return (int)ntohs(a.sin_port);
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

/* --- the TURN relay (guest side; called by the worker under s_lock) --- */

static int turn_wait_fallback(int i, ULONGLONG now); /* the hub room if set, else drop (below) */

/* a peer is in LP_TURN_WAIT (its punch failed): the worker should mint a credential. Cleared on read. */
int fci_nic_list_turn_need_take(void) { return InterlockedExchange(&s_turn_need, 0); }

/* the TURN allocation is ready and the peer is relayed: the worker should post the relayed
 * connect. Cleared on read. */
int fci_nic_list_turn_ready_take(void) { return InterlockedExchange(&s_turn_ready, 0); }

/* the nonce of the first LP_TURN_WAIT peer (the one the allocation is for); 0 if none */
int fci_nic_list_turn_wait(uint8_t nonce[16])
{
    int i;
    for (i = 0; i < s_nlpeers; i++)
        if (s_lpeers[i].state == LP_TURN_WAIT) {
            memcpy(nonce, s_lpeers[i].nonce, 16);
            return 1;
        }
    return 0;
}

/* the NIC's contribution to the browse status (Q29 round 4): in_list, n_pending, n_dropped.
 * Called from the game thread (the relabel gate); reads under s_lock. */
void fci_nic_browse_state(int *in_list, int *n_pending, int *n_dropped)
{
    AcquireSRWLockShared(&s_lock);
    *in_list = s_list;
    *n_pending = s_lpending;
    *n_dropped = (int)s_lpeer_dropped;
    ReleaseSRWLockShared(&s_lock);
}

/* clear the dropped counter (Q29 round 4): called when a fetch lists zero hosts, or when a peer
 * goes live. Called from the worker thread; takes s_lock. */
void fci_nic_browse_clear_dropped(void)
{
    AcquireSRWLockExclusive(&s_lock);
    s_lpeer_dropped = 0;
    ReleaseSRWLockExclusive(&s_lock);
}

/* start the allocation for the waiting peer (the worker's POST /v1/turn answered). The resolve is
 * the module's only blocking call; from the worker thread that is fine. 1 when the Allocate went out. */
int fci_nic_list_turn_start(const char *hostport, const char *user, const char *pass)
{
    int i;
    if (!s_turn_ok || fci_turn_state() != FCI_TURN_IDLE)
        return 0;
    for (i = 0; i < s_nlpeers; i++)
        if (s_lpeers[i].state == LP_TURN_WAIT)
            break;
    if (i >= s_nlpeers)
        return 0;
    return fci_turn_start(hostport, user, pass);
}

/* 0: allocating, 1: ready (the relayed address in ep), 2: failed. The ready/failed outcome is also
 * applied to the waiting peer by the tick (fci_turn_stop on failure). */
int fci_nic_list_turn_poll(char *ep, int len)
{
    struct sockaddr_in a;
    int st = fci_turn_state();
    if (st == FCI_TURN_READY && fci_turn_relayed(&a)) {
        snprintf(ep, (size_t)len, "%s:%u", inet_ntoa(a.sin_addr), ntohs(a.sin_port));
        return 1;
    }
    return st == FCI_TURN_FAILED ? 2 : 0;
}

/* the mint was refused (503/403/429): the waiting peer falls back to the hub relay, if set */
void fci_nic_list_turn_fail(void)
{
    int i;
    for (i = 0; i < s_nlpeers; i++)
        if (s_lpeers[i].state == LP_TURN_WAIT) {
            turn_wait_fallback(i, GetTickCount64());
            return;
        }
}

/* the guest's relayed connect arrived (the host side): the peer with this nonce is live at the
 * relayed address. It may be punching (LP_PENDING), waiting (LP_TURN_WAIT) or in a hub room
 * (LP_RELAY, which it leaves); a nonce no peer has is added as a live peer. 1 when it is live. */
int fci_nic_list_turn_peer(const uint8_t nonce[16], const uint8_t mac[6], const char *endpoint)
{
    struct addrinfo hints, *ai = NULL;
    char host[128], port[16];
    const char *colon;
    ULONGLONG now = GetTickCount64();
    int i;
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
    for (i = 0; i < s_nlpeers; i++) {
        lpeer_t *p = &s_lpeers[i];
        struct sockaddr_in ep;
        if (memcmp(p->nonce, nonce, 16))
            continue;
        ep = *(const struct sockaddr_in *)ai->ai_addr;
        if (p->state == LP_PENDING)
            s_lpending--;
        if (p->state == LP_RELAY && p->relay >= 0) /* it had fallen back to a hub room: leave it */
            relay_socket_close(p->relay);
        memcpy(&p->endpoint, &ep, sizeof p->endpoint);
        memcpy(p->mac, mac, 6);
        p->state = LP_LIVE;
        s_lpeer_dropped = 0; /* a peer going live clears the dropped counter (Q29 r4) */
        p->relay = -1;
        p->turn_ch = -1;
        p->via_turn = 1; /* its endpoint is the guest's relayed address */
        p->has_local = 0;
        p->next_punch = now; /* the tick opens our NAT to the relayed address */
        p->last_frame = now;
        freeaddrinfo(ai);
        fprintf(stderr, "[FCI] network: guest now at relay %s:%u (nonce %02x%02x%02x%02x)\n", inet_ntoa(ep.sin_addr),
                ntohs(ep.sin_port), nonce[0], nonce[1], nonce[2], nonce[3]);
        fflush(stderr);
        return 1;
    }
    if (s_nlpeers >= LIST_PEER_MAX) {
        freeaddrinfo(ai);
        return 0;
    }
    {
        lpeer_t *p = &s_lpeers[s_nlpeers];
        memcpy(&p->endpoint, ai->ai_addr, sizeof p->endpoint);
        freeaddrinfo(ai);
        memcpy(p->mac, mac, 6);
        memcpy(p->nonce, nonce, 16);
        p->state = LP_LIVE;
        p->guest = 0;
        p->relay = -1;
        p->turn_ch = -1;
        p->via_turn = 1; /* its endpoint is the guest's relayed address */
        p->has_local = 0;
        p->next_punch = now; /* the tick opens our NAT to the relayed address */
        p->last_frame = now;
        s_nlpeers++;
        fprintf(stderr, "[FCI] network: guest now at relay %s (nonce %02x%02x%02x%02x)\n", endpoint, nonce[0], nonce[1],
                nonce[2], nonce[3]);
        fflush(stderr);
        return 1;
    }
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
    for (n = 0; n < 16; n++) /* the room: the peer's nonce in hex, scoped by the triple */
        sprintf(s_relroom[i] + n * 2, "%02x", p->nonce[n]);
    s_relroom[i][32] = 0;
    room_scope(s_relroom[i], sizeof s_relroom[i]); /* the nonce is cut to fit the scope */
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
                if (e == WSAEMSGSIZE) {
                    oversize_drop();
                    continue;
                }
                if (e == WSAECONNRESET)
                    continue;
                break;
            }
            /* the socket is connected to the hub: only the hub's datagrams arrive */
            if (n < 5 || memcmp(buf, MAGIC, 4) || fromlen != sizeof from
                || from.sin_addr.s_addr != s_relay_hub.sin_addr.s_addr
                || from.sin_port != s_relay_hub.sin_port) {
                baddgram_log(&from, n);
                continue;
            }
            if (buf[4] == 'C' && n == 5 + COOKIE) {
                memcpy(s_relcookie[i], buf + 5, COOKIE);
                relay_hello(i); /* say hello with the cookie, as in hub mode */
                continue;
            }
            if (buf[4] != 'F' || n < 5 + 14 || n > 5 + ETHER_MAX) {
                baddgram_log(&from, n);
                continue;
            }
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
            if (rx_enqueue((const uint8_t *)buf + 5, n - 5, &from))
                traffic_rx(TP_HUB, n - 5);
        }
    }
}

/* relayed frames over TURN (the guest side): drain the module's queue into the card's rx queue,
 * accepted only from the relayed peer with its pre-seeded source MAC (as the hub rooms do) */
static void receive_turn(void)
{
    uint8_t d[FRAME_MAX + 8];
    struct sockaddr_in from;
    int n;
    while ((n = fci_turn_recv(d, sizeof d, &from)) > 0) {
        int i, ok = 0;
        const uint8_t *f = d + 5;
        /* the host's NAT opener (a punch, 21 bytes) carries no frame */
        if (n == 21 && !memcmp(d, MAGIC, 4) && d[4] == 'P')
            continue;
        /* the host's card datagram (FCN1 'F' + frame) */
        if (n < 5 + 14 || n > 5 + ETHER_MAX || memcmp(d, MAGIC, 4) || d[4] != 'F') {
            baddgram_log(&from, n);
            continue;
        }
        n -= 5;
        for (i = 0; i < s_nlpeers; i++) {
            lpeer_t *p = &s_lpeers[i];
            if (p->state != LP_RELAY || p->turn_ch < 0)
                continue;
            /* the peer's address as fci_turn reports it (the host's real address; a NAT rebind may
             * move the port), then the pre-seeded source MAC, as receive_relays does */
            p->endpoint = from;
            if (!(f[6] & 1) && !memcmp(p->mac, f + 6, 6)) { /* the source: no card sends from a group address */
                ok = 1;
                p->last_frame = GetTickCount64();
            }
            break; /* the guest has one relayed peer */
        }
        if (!ok) {
            if (!s_stray++) {
                int k, st = -1;
                const uint8_t *pm = NULL;
                for (k = 0; k < s_nlpeers; k++)
                    if (s_lpeers[k].turn_ch >= 0) {
                        st = s_lpeers[k].state;
                        pm = s_lpeers[k].mac;
                    }
                fprintf(stderr,
                        "[FCI] network: a relayed frame from no TURN peer, dropped (logged once): src "
                        "%02X:%02X:%02X:%02X:%02X:%02X, turn peer state %d mac %02X:%02X:%02X:%02X:%02X:%02X\n",
                        f[6], f[7], f[8], f[9], f[10], f[11], st, pm ? pm[0] : 0, pm ? pm[1] : 0, pm ? pm[2] : 0,
                        pm ? pm[3] : 0, pm ? pm[4] : 0, pm ? pm[5] : 0);
            }
            fflush(stderr);
            continue;
        }
        if (rx_enqueue(f, n, &from))
            traffic_rx(TP_TURN, n);
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

/* 1 when the datagram's source is a relayed guest's address (the host side of a TURN relay) */
static int from_relayed(const struct sockaddr_in *from)
{
    int i;
    for (i = 0; s_list && i < s_nlpeers; i++)
        if (s_lpeers[i].via_turn && s_lpeers[i].endpoint.sin_addr.s_addr == from->sin_addr.s_addr
            && s_lpeers[i].endpoint.sin_port == from->sin_port)
            return 1;
    return 0;
}

static void receive(void)
{
    char buf[FRAME_MAX + 8];
    int n, k;
    for (k = 0; k < 4 * RX_QUEUE; k++) { /* bounded: the rest waits for the next millisecond */
        struct sockaddr_in from;
        int fromlen = sizeof from;
        peer_t *p = NULL;
        n = recvfrom(s_sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
        if (n == SOCKET_ERROR) {
            int e = WSAGetLastError(); /* an oversized datagram or an ICMP report: skip it, not the rest */
            if (e == WSAEMSGSIZE) {
                oversize_drop();
                continue;
            }
            if (e == WSAECONNRESET)
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
            if (n < 5 || memcmp(buf, MAGIC, 4)) {
                baddgram_log(&from, n);
                continue;
            }
            if ((buf[4] == 'P' || buf[4] == 'A') && n == 21) {
                if (fromlen == sizeof from)
                    punch_recv(buf, buf[4], &from);
                continue;
            }
            if (buf[4] != 'F' || n < 5 + 14 || n > 5 + ETHER_MAX) {
                baddgram_log(&from, n);
                continue;
            }
            /* a frame: only from a live peer's endpoint, with that peer's MAC. A relayed guest's
             * endpoint is its relayed address, which is where the TURN server sends its frames from. */
            {
                int i;
                lpeer_t *lp = NULL;
                const uint8_t *src = (const uint8_t *)buf + 5 + 6;
                for (i = 0; i < s_nlpeers; i++) {
                    lpeer_t *c = &s_lpeers[i];
                    if (c->state != LP_LIVE)
                        continue;
                    if (c->endpoint.sin_addr.s_addr == from.sin_addr.s_addr
                        && c->endpoint.sin_port == from.sin_port) {
                        lp = c; /* a direct peer: the source is its endpoint */
                        break;
                    }
                }
                if (!lp) {
                    stray(&from);
                    continue;
                }
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
        } else if (n < 5 || memcmp(buf, MAGIC, 4)) {
            baddgram_log(&from, n);
            continue;
        } else if (s_lan) {
            /* LAN mode: the segment's cards. A knock ('H', 17 bytes) admits its sender's MAC when
             * the triple matches; a frame reaches the guest only from an admitted MAC. Our own
             * broadcast comes back to us (another adapter, the test port): its source MAC is the
             * card's own, drop it. The frame's size is checked before its source MAC is read, so a
             * short datagram cannot be read past its end (q24) */
            static ULONGLONG next_own;
            if (fromlen != sizeof from) {
                stray(&from);
                continue;
            }
            if (buf[4] == 'H' && n == 5 + 6 + NET_TRIPLE_LEN) {
                /* a knock's MAC follows the kind (buf + 5) */
                const uint8_t *kmac = (const uint8_t *)buf + 5;
                if (!(kmac[0] & 1) && memcmp(kmac, s_mac, 6)) { /* our own knock echoes: dropped */
                    if (net_triple_ok((const uint8_t *)buf + 11))
                        lan_admit(kmac, &from);
                    else
                        net_triple_mismatch(kmac, (const uint8_t *)buf + 11);
                }
                continue;
            }
            if (buf[4] != 'F' || n < 5 + 14 || n > 5 + ETHER_MAX) {
                baddgram_log(&from, n); /* an out-of-size F is an envelope drop, not a stranger */
                continue;
            }
            /* the frame's size was checked above, so its source MAC is in bounds (q24) */
            const uint8_t *src = (const uint8_t *)buf + 5 + 6;
            if (src[0] & 1) { /* no card sends from a group address */
                stray(&from);
                continue;
            }
            if (!memcmp(src, s_mac, 6)) {
                ULONGLONG now = GetTickCount64();
                if (now >= next_own) {
                    next_own = now + 5000; /* the segment echoes our own frames: once a few seconds */
                    fprintf(stderr,
                            "[FCI] network: LAN dropped own broadcast (%02X:%02X:%02X:%02X:%02X:%02X)\n",
                            src[0], src[1], src[2], src[3], src[4], src[5]);
                    fflush(stderr);
                }
                continue;
            }
            p = lan_frame_seen(src, &from); /* only a MAC admitted by a knock, while its knocks run */
            if (!p) {
                stray(&from);
                continue;
            }
        } else if (s_direct) { /* only peers reach the guest, each with its own card's frames */
            if (fromlen == sizeof from && buf[4] == 'H' && n == 5 + 6 + NET_TRIPLE_LEN) {
                /* a knock from a configured peer: the matching triple admits it, and updates its
                 * MAC, which changes across a restart of the peer's process */
                peer_t *k = peer_at(&from);
                if (k) {
                    if (net_triple_ok((const uint8_t *)buf + 11)) {
                        memcpy(k->mac, (const uint8_t *)buf + 5, 6);
                        k->has_mac = 1;
                        k->last_knock = GetTickCount64();
                    } else
                        net_triple_mismatch((const uint8_t *)buf + 5, (const uint8_t *)buf + 11);
                }
                continue;
            }
            p = fromlen == sizeof from ? peer_at(&from) : NULL;
            if (!p || buf[4] != 'F') {
                if (p)
                    baddgram_log(&from, n); /* a known peer sent a non-frame: the envelope's drop */
                stray(&from);
                continue;
            }
        } else if (buf[4] == 'C' && n == 5 + COOKIE) { /* the hub's cookie: say hello with it now */
            memcpy(s_cookie, buf + 5, COOKIE);
            hello();
            continue;
        }
        if (buf[4] != 'F' || n < 5 + 14 || n > 5 + ETHER_MAX) {
            baddgram_log(&from, n);
            continue;
        }
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
                /* the MAC comes from the peer's knock (direct); a frame from an unknocked or a
                 * different MAC is dropped */
                stray(&from);
                continue;
            }
        }
        if (rx_enqueue((const uint8_t *)buf + 5, n - 5, &from))
            traffic_rx(from_relayed(&from) ? TP_TURN : TP_DIRECT, n - 5);
    }
}

/* a TURN_WAIT peer that never got its relay: the hub room if set (a broken hub socket puts it
 * back to punching, as the punch path does), else dropped. 1 when the peer is kept at i, 0 when
 * it was removed (the table shifted: the caller steps back). */
static int turn_wait_fallback(int i, ULONGLONG now)
{
    lpeer_t *p = &s_lpeers[i];
    if (s_relay_spec[0]) {
        int r = relay_socket_new(p);
        if (r == -1) {
            fprintf(stderr,
                    "[FCI] network: relay socket cap (%d) reached, "
                    "dropping peer %s:%u\n",
                    RELAY_MAX, inet_ntoa(p->endpoint.sin_addr), ntohs(p->endpoint.sin_port));
            fflush(stderr);
            lpeer_remove(i);
            return 0;
        }
        if (r == -2) {
            /* no socket: back to punching, try again at the next window */
            p->state = LP_PENDING;
            p->punch_end = now + PUNCH_WINDOW_MS;
            p->next_punch = now;
            s_lpending++;
            fprintf(stderr, "[FCI] network: turn relay for %s:%u failed, no hub socket: back to punching\n",
                    inet_ntoa(p->endpoint.sin_addr), ntohs(p->endpoint.sin_port));
            fflush(stderr);
            return 1;
        }
        p->state = LP_RELAY;
        p->relay = r;
        p->last_frame = now;
        fprintf(stderr, "[FCI] network: turn relay for %s:%u failed, using relay room %.8s\n",
                inet_ntoa(p->endpoint.sin_addr), ntohs(p->endpoint.sin_port), relay_room_hex(p));
        fflush(stderr);
        return 1;
    }
    fprintf(stderr, "[FCI] network: turn relay for %s:%u failed, no relay set: peer dropped\n",
            inet_ntoa(p->endpoint.sin_addr), ntohs(p->endpoint.sin_port));
    fflush(stderr);
    InterlockedIncrement(&s_lpeer_dropped);
    lpeer_remove(i);
    return 0;
}

/* LIST mode: the punch schedule, the TURN relay, STUN refresh, and peer expiries (called under
 * s_lock in the tick) */
static void list_tick(ULONGLONG now)
{
    int i;
    /* TURN: retransmits, the half-LIFETIME refresh, the permission and channel re-issues, the drain */
    fci_turn_tick(now);
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
        /* a relayed guest's frames come from the TURN server's relayed address: our NAT lets them
         * in only once we have sent there, and the game sends nothing until it hears the guest */
        if (p->state == LP_LIVE && p->via_turn && now >= p->next_punch) {
            punch_send(p);
            p->next_punch = now + TURN_OPEN_MS;
        }
        if (p->state == LP_PENDING) {
            if (now >= p->punch_end) {
                /* 3 s with no answer: the TURN relay (the guest asks the list for a credential) */
                if (s_turn_ok && p->guest) {
                    p->state = LP_TURN_WAIT;
                    p->relay = -1;
                    p->turn_ch = -1;
                    s_lpending--;
                    p->last_frame = now;
                    p->punch_end = now + PEER_EXPIRE_MS;
                    fprintf(stderr, "[FCI] network: punch to %s:%u failed, asking the list for a relay\n",
                            inet_ntoa(p->endpoint.sin_addr), ntohs(p->endpoint.sin_port));
                    fflush(stderr);
                    InterlockedExchange(&s_turn_need, 1);
                    continue;
                }
                /* the host side, online: the guest asks for the relay and its relayed connect brings
                 * the peer back (fci_nic_list_turn_peer), so this attempt just ends */
                if (s_turn_ok && !p->guest && !s_relay_spec[0]) {
                    fprintf(stderr, "[FCI] network: punch to %s:%u failed, waiting for the guest's relay\n",
                            inet_ntoa(p->endpoint.sin_addr), ntohs(p->endpoint.sin_port));
                    fflush(stderr);
                    lpeer_remove(i);
                    i--;
                    continue;
                }
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
                InterlockedIncrement(&s_lpeer_dropped);
                lpeer_remove(i);
                i--;
                continue;
            }
            if (now >= p->next_punch) {
                punch_send(p);
                p->next_punch += PUNCH_PERIOD_MS;
            }
        } else if (p->state == LP_TURN_WAIT) {
            /* the mint and the allocation had 15 s; the outcome is applied below (READY/FAILED) */
            if (now >= p->punch_end && fci_turn_state() != FCI_TURN_READY
                && fci_turn_state() != FCI_TURN_FAILED) {
                if (!turn_wait_fallback(i, now))
                    i--;
                continue;
            }
        } else if (now - p->last_frame > PEER_EXPIRE_MS) {
            /* LP_LIVE and LP_RELAY: expire after 15 s without a frame */
            if (p->state == LP_RELAY && p->relay >= 0) {
                fprintf(stderr, "[FCI] network: relay room %.8s: no frame in 15 s, peer expired\n",
                        relay_room_hex(p));
                relay_socket_close(p->relay);
                p->relay = -1;
            } else if (p->state == LP_RELAY && p->turn_ch >= 0) {
                fprintf(stderr, "[FCI] network: turn relay to %s:%u: no frame in 15 s, peer expired\n",
                        inet_ntoa(p->endpoint.sin_addr), ntohs(p->endpoint.sin_port));
            } else {
                fprintf(stderr, "[FCI] network: peer %s:%u: no frame in 15 s, expired\n",
                        inet_ntoa(p->endpoint.sin_addr), ntohs(p->endpoint.sin_port));
            }
            fflush(stderr);
            lpeer_remove(i);
            i--;
        }
    }
    /* the TURN outcome: ready moves the waiting peer to the relay (the worker posts the relayed
     * connect); failed falls back to the hub room, if set */
    {
        int st = fci_turn_state();
        if (st == FCI_TURN_READY || st == FCI_TURN_FAILED) {
            int i, j, used = 0;
            for (i = 0; i < s_nlpeers; i++)
                if (s_lpeers[i].state == LP_TURN_WAIT)
                    break;
            for (j = 0; j < s_nlpeers; j++)
                if (s_lpeers[j].state == LP_RELAY && s_lpeers[j].turn_ch >= 0)
                    used = 1;
            if (i >= s_nlpeers) {
                /* nobody waits; the allocation lives while a relayed peer uses it */
                if (!used)
                    fci_turn_stop();
            } else if (st == FCI_TURN_READY) {
                lpeer_t *p = &s_lpeers[i];
                struct sockaddr_in a;
                int ch = fci_turn_add_peer(&p->endpoint);
                if (ch < 0 || !fci_turn_relayed(&a)) {
                    fci_turn_stop();
                    if (!turn_wait_fallback(i, now))
                        i--;
                } else {
                    p->state = LP_RELAY;
                    p->turn_ch = ch;
                    p->relay = -1;
                    p->last_frame = now;
                    fprintf(stderr, "[FCI] network: turn ready, relayed %s:%u\n", inet_ntoa(a.sin_addr),
                            ntohs(a.sin_port));
                    fflush(stderr);
                    InterlockedExchange(&s_turn_ready, 1);
                }
            } else {
                fci_turn_stop();
                if (!turn_wait_fallback(i, now))
                    i--;
            }
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
        if (s_lan && now >= s_lan_bc_refresh) {
            lan_adapters_refresh(); /* Wi-Fi changes: the list is re-read every 30 s */
            s_lan_bc_refresh = now + LAN_REFRESH_MS;
        }
        if (s_lan && now >= s_lan_knock_next) {
            lan_knock(); /* the segment's cards admit each other on it (the triple checked) */
            s_lan_knock_next = now + LAN_KNOCK_MS;
        }
        if (s_direct && now >= s_dir_knock_next) {
            direct_knock(); /* the fixed peers admit us on it, as we admit them */
            s_dir_knock_next = now + LAN_KNOCK_MS;
        }
        traffic_summary(now, 0);
        if (s_list) {
            list_tick(now);
            receive_relays(); /* before receive(): the relay queue shares s_rxq */
            receive_turn();   /* the TURN relay: the same queue */
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
    room_scope(s_room, sizeof s_room); /* the triple in the room name: mismatched builds never meet */
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
    s_dir_knock_next = GetTickCount64() + LAN_KNOCK_MS; /* the first knock a second in */
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
    const char *endpoint = getenv("RECOMP_FCI_NET_ENDPOINT"), *relay = getenv("RECOMP_FCI_NET_RELAY"),
               *turn = getenv("RECOMP_FCI_NET_TURN");
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
    s_turn_ok = !(turn && turn[0] == '0'); /* the TURN relay, on unless disabled */
    {
        const char *punch = getenv("RECOMP_FCI_NET_PUNCH");
        s_punch_off = punch && punch[0] == '0';
        if (s_punch_off)
            fprintf(stderr, "[FCI] network: hole punching disabled (RECOMP_FCI_NET_PUNCH=0): a test of the relay\n");
    }
    if (!s_turn_ok)
        fprintf(stderr, "[FCI] network: TURN relay disabled (RECOMP_FCI_NET_TURN=0)\n");
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

/* one more subnet broadcast, de-duplicated (several addresses on one adapter share it); 0 when the
 * list is full */
static int lan_add_bc(const struct in_addr *bc)
{
    int i;
    for (i = 0; i < s_lan_nbc; i++)
        if (s_lan_bc[i].s_addr == bc->s_addr)
            return 1;
    if (s_lan_nbc >= 8)
        return 0;
    s_lan_bc[s_lan_nbc++] = *bc;
    return 1;
}

/* 1 when the two de-duplicated broadcast sets differ (same size, same members) */
static int lan_bc_set_changed(const struct in_addr *old, int oldn, const struct in_addr *now, int nown)
{
    int i, j;
    if (oldn != nown)
        return 1;
    for (i = 0; i < nown; i++) {
        for (j = 0; j < oldn; j++)
            if (old[j].s_addr == now[i].s_addr)
                break;
        if (j == oldn)
            return 1;
    }
    return 0;
}

/* the broadcast send list: the subnet broadcast (network | ~mask) of each up IPv4 adapter, skipping
 * loopback, down adapters (0.0.0.0/0) and 169.254/16 link-local (unless it is the only one). Called at
 * start, on a 30 s timer and on a send failure (Wi-Fi changes). The list is logged at start; a refresh
 * logs only when the set of broadcast addresses changed. Uses the classic GetAdaptersInfo (the Vista
 * GetAdaptersAddresses is not in the older mingw headers the build is checked against). */
static void lan_adapters_refresh(void)
{
    IP_ADAPTER_INFO *a = NULL;
    ULONG len = sizeof *a;
    struct in_addr oldbc[8];
    int oldn, ll = 0, got = 0;
    struct in_addr llbc = {0};
    int first = !s_lan_bc_logged;

    memcpy(oldbc, s_lan_bc, sizeof oldbc);
    oldn = s_lan_nbc;
    s_lan_nbc = 0; /* recompute the list from scratch */

    if (GetAdaptersInfo(NULL, &len) != ERROR_BUFFER_OVERFLOW) {
        fprintf(stderr, "[FCI] network: LAN adapter list failed (WSA %d); no broadcast addresses\n",
                WSAGetLastError());
        fflush(stderr);
    } else {
        a = (IP_ADAPTER_INFO *)malloc(len);
        if (!a || GetAdaptersInfo(a, &len) != NO_ERROR) {
            fprintf(stderr, "[FCI] network: LAN adapter list failed (WSA %d); no broadcast addresses\n",
                    WSAGetLastError());
            fflush(stderr);
        } else {
            got = 1;
            for (; a; a = a->Next) {
                struct in_addr host = {0}, mask = {0}, bcn;
                uint32_t h, m;
                PIP_ADDR_STRING ip = a->CurrentIpAddress ? a->CurrentIpAddress : &a->IpAddressList;
                if (a->Type == MIB_IF_TYPE_LOOPBACK || !ip || !ip->IpAddress.String[0])
                    continue;
                if (inet_pton(AF_INET, ip->IpAddress.String, &host) != 1)
                    continue;
                if (ip->IpMask.String[0])
                    inet_pton(AF_INET, ip->IpMask.String, &mask);
                h = ntohl(host.s_addr);
                m = ntohl(mask.s_addr);
                if (!h || !m)
                    continue; /* a down adapter reports 0.0.0.0/0: no broadcast to send to */
                bcn.s_addr = htonl((h & m) | ~m);
                if ((h & 0xFFFF0000u) == 0xA9FE0000u) { /* 169.254/16: only the last one counts */
                    if (!ll) {
                        ll = 1;
                        llbc = bcn;
                    }
                    continue;
                }
                lan_add_bc(&bcn);
                if (first) {
                    char hb[INET_ADDRSTRLEN], bb[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &host, hb, sizeof hb);
                    inet_ntop(AF_INET, &bcn, bb, sizeof bb);
                    fprintf(stderr, "[FCI] network: LAN adapter %s %s -> broadcast %s\n",
                            a->AdapterName[0] ? a->AdapterName : "adapter", hb, bb);
                    fflush(stderr);
                }
            }
            if (!s_lan_nbc && ll) {
                lan_add_bc(&llbc);
                if (first) {
                    char bb[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &llbc, bb, sizeof bb);
                    fprintf(stderr, "[FCI] network: LAN adapter (link-local only) -> broadcast %s\n", bb);
                    fflush(stderr);
                }
            }
        }
    }
    free(a);
    if (got && !first && lan_bc_set_changed(oldbc, oldn, s_lan_bc, s_lan_nbc))
        fprintf(stderr, "[FCI] network: LAN adapters changed: %d broadcast addresses\n", s_lan_nbc),
            fflush(stderr);
    s_lan_bc_logged = 1;
}

/* the env path (at boot) and the menu (later) share one starter per transport; each is
 * idempotency-guarded (a transport already up is left alone, the first wins) and logs the link up.
 * 1 when the link is up. */
int fci_nic_start_hub(const char *spec)
{
    uint8_t m[6];
    if (s_sock != INVALID_SOCKET) {
        fprintf(stderr, "[FCI] network: a transport is already up; the hub is not started\n");
        fflush(stderr);
        return 0;
    }
    if (!connect_hub(spec)) {
        fprintf(stderr, "[FCI] RECOMP_FCI_NET=%s: not <host>:<port>[/<room>], or no socket (%d)\n", spec,
                WSAGetLastError());
        fflush(stderr);
        return 0;
    }
    fci_nic_mac(m);
    fprintf(stderr, "[FCI] network: link up through hub %s, card %02X:%02X:%02X:%02X:%02X:%02X\n", spec,
            m[0], m[1], m[2], m[3], m[4], m[5]);
    fflush(stderr);
    return 1;
}

int fci_nic_start_peers(const char *spec)
{
    char err[128];
    uint8_t m[6];
    if (s_sock != INVALID_SOCKET) {
        fprintf(stderr, "[FCI] network: a transport is already up; the peers are not started\n");
        fflush(stderr);
        return 0;
    }
    if (!connect_peers(spec, err, sizeof err)) {
        fprintf(stderr, "[FCI] RECOMP_FCI_NET_PEERS=%s: %s\n", spec, err);
        fflush(stderr);
        return 0;
    }
    fci_nic_mac(m);
    fprintf(stderr, "[FCI] network: link up to %d peers, card %02X:%02X:%02X:%02X:%02X:%02X\n", s_npeers,
            m[0], m[1], m[2], m[3], m[4], m[5]);
    fflush(stderr);
    return 1;
}

int fci_nic_start_list(const char *url)
{
    char err[128];
    uint8_t m[6];
    if (s_sock != INVALID_SOCKET) {
        fprintf(stderr, "[FCI] network: a transport is already up; the list is not started\n");
        fflush(stderr);
        return 0;
    }
    if (!connect_list(url, err, sizeof err)) {
        fprintf(stderr, "[FCI] RECOMP_FCI_NET_LIST=%s: %s\n", url, err);
        fflush(stderr);
        return 0;
    }
    fci_nic_mac(m);
    fprintf(stderr, "[FCI] network: link up (host list %s), card %02X:%02X:%02X:%02X:%02X:%02X\n", url,
            m[0], m[1], m[2], m[3], m[4], m[5]);
    fflush(stderr);
    fci_net_worker_start(); /* LIST mode: the worker thread does the host-list HTTP */
    /* leave the host list on a clean exit (quit-to-dashboard / shutdown): the toolkit calls
     * this once, on the exiting thread, just before it exits (patches/runtime/0034-exit-hook.patch) */
    extern void (*xbox_exit_hook)(void);
    xbox_exit_hook = fci_net_leave;
    return 1;
}

/* LAN mode (RECOMP_FCI_NET_LAN=1, fci.ini network-lan): the card's frames are carried by plain UDP
 * broadcast on the players' own L2 segment (the subnet broadcast of each up IPv4 adapter); no hub,
 * no list, nothing leaves the segment. The game's ~1/s broadcast is the keepalive, so no hello.
 * Idempotent: a second start (the menu, later) logs and leaves the first transport. 1 when up. */
int fci_nic_start_lan(void)
{
    WSADATA wsa;
    u_long nb = 1;
    uint16_t port = LAN_PORT_DEFAULT;
    const char *pe = getenv("RECOMP_FCI_NET_LAN_PORT"), *bp = getenv("RECOMP_FCI_NET_LAN_BCPORTS");
    uint8_t m[6];
    if (s_sock != INVALID_SOCKET) {
        fprintf(stderr, "[FCI] network: a transport is already up; the LAN transport is not started\n");
        fflush(stderr);
        return 0;
    }
    if (pe && *pe) {
        int p = atoi(pe);
        if (p <= 0 || p > 65535) {
            fprintf(stderr, "[FCI] RECOMP_FCI_NET_LAN_PORT=%s is not 1-65535\n", pe);
            fflush(stderr);
            return 0;
        }
        port = (uint16_t)p;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        fprintf(stderr, "[FCI] network: LAN WSAStartup failed (%d); link down\n", WSAGetLastError());
        fflush(stderr);
        return 0;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) {
        fprintf(stderr, "[FCI] network: LAN no socket (%d); link down\n", WSAGetLastError());
        fflush(stderr);
        return 0;
    }
    {
        int on = 1;
        setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on); /* a bind to any is no
                                                                                      broadcast right */
    }
    {
        struct sockaddr_in local;
        memset(&local, 0, sizeof local);
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = htons(port);
        if (bind(s_sock, (struct sockaddr *)&local, sizeof local)) {
            int e = WSAGetLastError();
            closesocket(s_sock);
            s_sock = INVALID_SOCKET;
            fprintf(stderr, "[FCI] network: LAN bind 0.0.0.0:%u failed (WSA %d); link down\n", port, e);
            fflush(stderr);
            return 0;
        }
    }
    if (ioctlsocket(s_sock, FIONBIO, &nb)) {
        int e = WSAGetLastError();
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        fprintf(stderr, "[FCI] network: LAN non-blocking mode failed (%d); link down\n", e);
        fflush(stderr);
        return 0;
    }
    /* the broadcast destination ports (RECOMP_FCI_NET_LAN_BCPORTS, the test hook): the default is the
     * bind port; the game's browser probes the port a match was listed on, the XNET card the segment's */
    s_lan_port = port;
    s_lan_nbcports = 0;
    if (bp && *bp) {
        char list[128];
        if (strlen(bp) < sizeof list) {
            char *tok;
            strcpy(list, bp);
            for (tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
                int p = atoi(tok);
                if (p <= 0 || p > 65535) {
                    fprintf(stderr, "[FCI] RECOMP_FCI_NET_LAN_BCPORTS: %s is not 1-65535, skipped\n", tok);
                    fflush(stderr);
                    continue;
                }
                if (s_lan_nbcports < 8)
                    s_lan_bcports[s_lan_nbcports++] = (uint16_t)p;
            }
        }
    }
    if (!s_lan_nbcports)
        s_lan_bcports[s_lan_nbcports++] = port;
    s_lan = 1;
    lan_adapters_refresh();
    s_lan_bc_refresh = GetTickCount64() + LAN_REFRESH_MS;
    s_lan_knock_next = GetTickCount64() + LAN_KNOCK_MS; /* the first knock a second in */
    fci_nic_mac(m);
    fprintf(stderr, "[FCI] network: LAN up, %02X:%02X:%02X:%02X:%02X:%02X, bound 0.0.0.0:%u\n", m[0], m[1],
            m[2], m[3], m[4], m[5], port);
    fflush(stderr);
    return 1;
}

/* returns 0 when the page cannot be trapped or RECOMP_FCI_NET(_PEERS|_LIST|_LAN) cannot be used */
int fci_nic_init(ptrdiff_t mem_offset)
{
    DWORD old;
    const char *net = getenv("RECOMP_FCI_NET"), *peers = getenv("RECOMP_FCI_NET_PEERS");
    const char *list = getenv("RECOMP_FCI_NET_LIST"), *lan = getenv("RECOMP_FCI_NET_LAN");
    const char *menu = getenv("RECOMP_FCI_NET_MENU"), *trace = getenv("RECOMP_FCI_NET_TRACE");
    int nset = (net && *net) + (peers && *peers) + (list && *list && !(menu && *menu == '1')) + (lan && *lan == '1');
    s_trace = trace && *trace == '1';
    s_mem_offset = mem_offset;
    s_menu = menu && *menu == '1';
    if (list && *list && strlen(list) < sizeof s_menu_list_url)
        strcpy(s_menu_list_url, list); /* the menu path's LIST URL (RECOMP_FCI_NET_LIST, overridable) */
    if (nset > 1) {
        fprintf(stderr, "[FCI] RECOMP_FCI_NET, _PEERS, _LIST and _LAN: one link at a time\n");
        return 0;
    }
    if (net && *net) {
        if (!fci_nic_start_hub(net))
            return 0;
    } else if (peers && *peers) {
        if (!fci_nic_start_peers(peers))
            return 0;
    } else if (list && *list && !s_menu) { /* in menu mode the URL waits for the INTERNET pick */
        if (!fci_nic_start_list_lazy(list))
            return 0;
    } else if (lan && *lan == '1') {
        if (!fci_nic_start_lan())
            return 0;
    } else if (s_menu) {
        /* menu mode, no transport at boot: the card is up (the online menu items are enabled) and the
         * player's pick starts the transport. The 30-updates lock stays off until a transport is up. */
        s_up = 1;
        fprintf(stderr, "[FCI] network: menu mode: link up (no transport; the player's pick starts it)\n");
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

/* the worker's probe flag: set by route_frame on broadcast, cleared by the worker after a fetch.
 * Returns 1 and clears the flag if a probe was pending. */
int fci_nic_worker_probe_take(void) { return (int)InterlockedExchange(&s_probe, 0); }

/* the list URL (for the worker). Returns NULL if not in LIST mode. */
const char *fci_nic_list_url(void) { return s_list ? s_list_url : NULL; }

/* the NIC's lock (the worker takes it for fci_nic_list_* calls) */
SRWLOCK *fci_nic_lock(void) { return &s_lock; }

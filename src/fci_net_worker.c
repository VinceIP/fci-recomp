/* The network worker thread: one WinHTTP session for the host list (RECOMP_FCI_NET_LIST).
 * Guest path: the NIC signals "probe" on broadcast; the worker fetches /v1/hosts, adds peers,
 * and posts /v1/connect for each new host.
 * Host path: fci_prehook_0051C5EA (XNetCreateKey: only a host creates a key) sets "host started"; the worker registers
 * and long-polls /v1/connects.
 * It never touches the card's UDP socket; it takes s_lock only around fci_nic_list_* calls.
 * research/docs/multiplayer.md, tools/fake_hostlist.py */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fci_image.h"
#include "fci_net.h"
#include "fci_net_worker.h"

/* the NIC's worker API (fci_nic.c): called under s_lock */
extern int fci_nic_list_add_peer(const char *endpoint, const char *local, const uint8_t mac[6],
                                 const uint8_t nonce[16], int guest, const char *receipt, ULONGLONG started);
extern int fci_nic_list_endpoint(char *buf, int buflen);
extern int fci_nic_list_local_port(void);
extern void fci_nic_mac(uint8_t *mac);
extern void fci_nic_mac_str(char *buf, int buflen);
extern int fci_nic_list_has_peer(const char *endpoint);
extern int fci_nic_worker_probe_take(void);
extern const char *fci_nic_list_url(void);
extern SRWLOCK *fci_nic_lock(void); /* the NIC's s_lock */
/* the TURN relay (fci_nic.c): the worker mints and posts, the NIC owns the allocation */
extern int fci_nic_list_turn_need_take(void);
extern int fci_nic_list_turn_ready_take(void);
extern int fci_nic_list_turn_wait(uint8_t *nonce);
extern int fci_nic_list_turn_start(const char *hostport, const char *user, const char *pass);
extern int fci_nic_list_turn_poll(char *ep, int len);
extern void fci_nic_list_turn_fail(void);
extern int fci_nic_list_turn_peer(const uint8_t nonce[16], const uint8_t mac[6], const char *endpoint);
extern void fci_nic_browse_clear_dropped(void); /* Q29 round 4: clear the dropped counter */

/* the XBE certificate's Version field (fci_main.c); the data folder (fci_main.c) */
extern uint32_t fci_xbe_version;
extern const char *fci_save_dir;

#define LIST_FETCH_INTERVAL_MS 10000
#define POLL_WAIT_S            20
#define WORKER_TICK_MS         500

static HANDLE s_thread;
static HANDLE s_stop;
static HINTERNET s_session;
static volatile LONG s_host_started; /* set by fci_prehook_0051C5EA */

/* the identity: 32 random bytes, hex (s_identity), stable across sessions. It lives in the data
 * folder (fci_save_dir\fci_token: %APPDATA%\fci-recomp\profile on Windows, the Wine prefix's
 * AppData under Proton). The player's id is the first 16 bytes of SHA-256(token), hex
 * (s_own_id): what the lobby bans and what the listing shows. A missing or unreadable token file
 * gets a fresh token written back; the worker never fails because of it (research/docs/
 * online-architecture.md, "The identity token"). */
static char s_identity[65];
static char s_own_id[33];

/* host registration state */
/* the card's LAN address (private IPv4:port), for the Worker's same-NAT forwarding. Detected
 * once at worker start: RECOMP_FCI_NET_LOCAL (testing override) first, then auto-detect (a UDP
 * connect to a public IP, getsockname for the local address, the card socket's port). */
static char s_local_addr[64];

static void log_line(const char *fmt, ...); /* forward declaration (defined below) */

/* a private IPv4 (10/8, 172.16/12, 192.168/16) with a valid port.
 * 100.64/10 (CGNAT) is excluded: two phones behind the same carrier share no network. */
static int local_addr_ok(const char *s)
{
    unsigned a, b, c, d, port;
    if (sscanf(s, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) != 5)
        return 0;
    if (a < 1 || a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    if (port == 0 || port > 65535)
        return 0;
    return (a == 10) || (a == 172 && b >= 16 && b <= 31) ||
           (a == 192 && b == 168);
}

static void local_addr_detect(void)
{
    if (s_local_addr[0])
        return;
    /* 1. RECOMP_FCI_NET_LOCAL (testing override) */
    const char *env = getenv("RECOMP_FCI_NET_LOCAL");
    if (env && env[0]) {
        if (local_addr_ok(env)) {
            snprintf(s_local_addr, sizeof s_local_addr, "%s", env);
            log_line("local address: %s (RECOMP_FCI_NET_LOCAL)", s_local_addr);
        } else {
            log_line("RECOMP_FCI_NET_LOCAL=%s is not a private IPv4:port, ignored", env);
        }
        return;
    }
    /* 2. auto-detect: UDP connect to the host-list server's IP (no data sent), getsockname for
     * the local address, combine with the card socket's bound port. The list server is the one
     * the card already talks to; resolving its host gives the outbound interface's local address.
     * If the host does not resolve, omit local. */
    int port = fci_nic_list_local_port();
    if (!port)
        return;
    const char *url = fci_nic_list_url();
    if (!url)
        return;
    /* extract the host from the URL (http://host:port/... or https://host/...) */
    char host[128];
    const char *slash = strstr(url, "://");
    if (!slash) {
        WSACleanup();
        return;
    }
    slash += 3;
    const char *end = strchr(slash, '/');
    size_t hlen = end ? (size_t)(end - slash) : strlen(slash);
    if (hlen >= sizeof host) {
        WSACleanup();
        return;
    }
    memcpy(host, slash, hlen);
    host[hlen] = 0;
    /* strip the port from the host (if present) */
    char *hcolon = strrchr(host, ':');
    if (hcolon)
        *hcolon = 0;
    /* resolve the host to an IPv4 address */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return;
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &ai) || !ai) {
        WSACleanup();
        return; /* the host does not resolve: omit local */
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(ai);
        WSACleanup();
        return;
    }
    struct sockaddr_in local;
    int len = sizeof local;
    if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0 &&
        !getsockname(s, (struct sockaddr *)&local, &len)) {
        unsigned a, b, c, d;
        char ip[32];
        snprintf(ip, sizeof ip, "%s", inet_ntoa(local.sin_addr));
        if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            int is_private = (a == 10) || (a == 172 && b >= 16 && b <= 31) ||
                             (a == 192 && b == 168);
            if (is_private) {
                snprintf(s_local_addr, sizeof s_local_addr, "%s:%u", ip, (unsigned)port);
                log_line("local address detected: %s", s_local_addr);
            }
        }
    }
    closesocket(s);
    freeaddrinfo(ai);
    WSACleanup();
}

static char s_host_id[64];
static int s_registered;
static ULONGLONG s_last_fetch_attempt;    /* last /v1/hosts attempt (GetTickCount64) */
static ULONGLONG s_host_start_ms; /* when "host started" was set (for the endpoint-wait log) */
static ULONGLONG s_reg_fail_ms;   /* last failed register (back off 5 s; a banned id gets 403 forever) */
static int s_endpoint_warned;     /* logged once if the endpoint is not known 5 s after host start */

/* the worker's contribution to the browse status (Q29 round 4) */
static volatile LONG s_fetch_state;    /* 0 = never fetched, 1 = last fetch ok, 2 = last fetch failed */
static volatile LONG s_fetch_nhosts;   /* number of hosts in the last successful fetch */
static volatile LONG s_relay_refused;  /* 1 when the last relay mint was refused (503); clears on success */
static volatile LONG s_version_refused; /* 1 when the list refused us for the version (not yet wired) */

static const char *net_title(void) { return strstr(FCI_TITLE_NAME, "Evolution") ? "evolution" : "instincts"; }

static const char *net_version(void)
{
    /* the XBE certificate's Version field: a patched disc lists apart from another */
    static char buf[16];
    snprintf(buf, sizeof buf, "%u", (unsigned)fci_xbe_version);
    return buf;
}

static void log_line(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[FCI] network: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* --- minimal JSON field extraction --- */

/* find "key" in buf, return a pointer to the value (after the colon and any whitespace) */
static const char *json_str(const char *buf, const char *key)
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(buf, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return NULL;
    return p + 1;
}

/* extract a string value: copies up to buflen-1 chars until the closing quote */
static int json_str_val(const char *buf, const char *key, char *out, int buflen)
{
    const char *p = json_str(buf, key);
    if (!p)
        return 0;
    {
        int n = 0;
        while (*p && *p != '"' && n < buflen - 1)
            out[n++] = *p++;
        out[n] = 0;
        return n > 0;
    }
}

/* extract a boolean value: 1 for true, 0 for false, -1 when absent */
static int json_bool(const char *buf, const char *key)
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(buf, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t')
        p++;
    if (strncmp(p, "true", 4) == 0)
        return 1;
    if (strncmp(p, "false", 5) == 0)
        return 0;
    return -1;
}

/* extract an array of objects: calls fn(obj_buf, obj_len) for each '{...}' in the array */
static void json_array(const char *buf, const char *key, void (*fn)(const char *, int))
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(buf, pat);
    if (!p)
        return;
    p += strlen(pat);
    while (*p && *p != '[')
        p++;
    if (*p != '[')
        return;
    p++;
    while (*p) {
        while (*p && *p != '{')
            p++;
        if (!*p)
            break;
        /* find the matching } */
        {
            const char *start = p;
            int depth = 0;
            while (*p) {
                if (*p == '{')
                    depth++;
                else if (*p == '}') {
                    depth--;
                    if (depth == 0) {
                        p++;
                        break;
                    }
                }
                p++;
            }
            fn(start, (int)(p - start));
        }
        while (*p && *p != '}')
            p++;
        if (*p == '}')
            p++;
        while (*p && *p != ']' && *p != '{')
            p++;
        if (*p == ']')
            break;
    }
}

/* --- WinHTTP helpers --- */

/* convert a UTF-8 string to a wide string in dst (up to dstlen wide chars) */
static void to_wide(const char *src, wchar_t *dst, int dstlen)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dstlen);
    if (n > 0)
        dst[n - 1] = 0;
    else
        dst[0] = 0;
}

/* parse a base URL ("http://host[:port]" or "https://host[:port]") into host, port and secure.
 * 0 on success, -1 on a bad scheme; host gets up to hostlen-1 chars. */
static int parse_base(const char *base_url, char *host, int hostlen, int *port, int *secure)
{
    const char *p = base_url;
    if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        *port = 443;
        *secure = 1;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        *port = 80;
        *secure = 0;
    } else {
        return -1;
    }
    /* find the end of host:port */
    {
        const char *slash = strchr(p, '/');
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen >= (size_t)hostlen)
            hlen = (size_t)hostlen - 1;
        memcpy(host, p, hlen);
        host[hlen] = 0;
        {
            char *colon = strrchr(host, ':');
            if (colon && strchr(colon + 1, '/') == NULL) {
                *colon = 0;
                *port = atoi(colon + 1);
            }
        }
    }
    return 0;
}

/* do a request on an open session; returns the response body in buf (up to buflen), 0 on success,
 * -1 on error. method: "GET" or "POST"; path: the URL path+query; body: POST body or NULL.
 * Returns the HTTP status code in *status. */
static int http_request_on(HINTERNET session, const char *host, int port, int secure, const char *method,
                           const char *path, const char *body, char *buf, int buflen, int *status)
{
    wchar_t wmethod[16], wpath[512], whost[256], wheaders[64];
    HINTERNET hconnect = NULL;
    HINTERNET hrequest = NULL;
    DWORD bytes = 0, total = 0;

    to_wide(method, wmethod, 16);
    to_wide(path, wpath, 512);
    to_wide(host, whost, 256);
    wcscpy(wheaders, L"Content-Type: application/json\r\n"); /* the Worker answers 415 to anything else */

    hconnect = WinHttpConnect(session, whost, (UINT)port, 0);
    if (!hconnect)
        return -1;
    hrequest = WinHttpOpenRequest(hconnect, wmethod, wpath, NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hrequest) {
        WinHttpCloseHandle(hconnect);
        return -1;
    }

    WinHttpAddRequestHeaders(hrequest, wheaders, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hrequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (void *)body,
                            body ? (DWORD)strlen(body) : 0, body ? (DWORD)strlen(body) : 0, 0)) {
        int err = GetLastError();
        log_line("winhttp send %s %s failed (%u)", method, path, (unsigned)err);
        WinHttpCloseHandle(hrequest);
        WinHttpCloseHandle(hconnect);
        return -1;
    }

    if (!WinHttpReceiveResponse(hrequest, NULL)) {
        int err = GetLastError();
        log_line("winhttp receive %s %s failed (%u)", method, path, (unsigned)err);
        WinHttpCloseHandle(hrequest);
        WinHttpCloseHandle(hconnect);
        return -1;
    }

    {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(hrequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, 0);
        *status = (int)code;
    }

    buf[0] = 0;
    while (total < (DWORD)buflen - 1) {
        if (!WinHttpQueryDataAvailable(hrequest, &bytes) || bytes == 0)
            break;
        if (bytes > (DWORD)(buflen - 1 - (int)total))
            bytes = (DWORD)(buflen - 1 - (int)total);
        if (!WinHttpReadData(hrequest, buf + total, bytes, &bytes))
            break;
        total += bytes;
    }
    buf[total] = 0;

    WinHttpCloseHandle(hrequest);
    WinHttpCloseHandle(hconnect);
    return 0;
}

/* do a request on the worker's session; see http_request_on. */
static int http_request(const char *base_url, const char *method, const char *path, const char *body,
                        char *buf, int buflen, int *status)
{
    char host[256];
    int port = 80, secure = 0;
    *status = 0; /* No HTTP response yet. */
    if (parse_base(base_url, host, (int)sizeof host, &port, &secure) != 0)
        return -1;
    return http_request_on(s_session, host, port, secure, method, path, body, buf, buflen, status);
}

/* do a request on a fresh session bounded to about budget_ms in total: WinHttpSetTimeouts splits the
 * budget across resolve, connect, send and receive, so the call waits at most about budget_ms. The
 * connect phase carries the TLS handshake, so it gets the largest share. The session is closed before
 * returning. Used on an exit path, where leaving the host list must not delay the exit (fci_net_leave). */
static int http_request_timed(const char *base_url, const char *method, const char *path, const char *body,
                              char *buf, int buflen, int *status, DWORD budget_ms)
{
    char host[256];
    int port = 80, secure = 0;
    HINTERNET session;
    int r;

    *status = 0; /* No HTTP response yet. */
    if (parse_base(base_url, host, (int)sizeof host, &port, &secure) != 0)
        return -1;
    session = WinHttpOpen(L"fci-recomp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return -1;
    /* resolve, connect (the TLS handshake), send, receive: they sum to the budget */
    WinHttpSetTimeouts(session, budget_ms / 5, 2 * budget_ms / 5, budget_ms / 5, budget_ms / 5);
    r = http_request_on(session, host, port, secure, method, path, body, buf, buflen, status);
    WinHttpCloseHandle(session);
    return r;
}

/* --- nonce generation --- */

static void gen_nonce(uint8_t nonce[16])
{ BCryptGenRandom(NULL, nonce, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG); }

/* --- the identity token (Q11) --- */

/* Load the identity token from the data folder; a missing, unreadable, or wrong-sized file gets a
 * fresh 32-byte token written back (never a failure), and the id is derived from the token. */
static void identity_init(void)
{
    uint8_t raw[32], hash[32];
    char path[MAX_PATH];
    FILE *f = NULL;
    int i;

    if (fci_save_dir && fci_save_dir[0])
        snprintf(path, sizeof path, "%s\\fci_token", fci_save_dir);
    else
        path[0] = 0;
    if (path[0] && (f = fopen(path, "rb")) != NULL && fread(raw, 1, sizeof raw, f) == sizeof raw) {
        fclose(f);
    } else {
        int wrote = 0;
        if (f)
            fclose(f);
        if (BCryptGenRandom(NULL, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
            return; /* never: the system RNG is always available */
        if (path[0] && (f = fopen(path, "wb")) != NULL) {
            wrote = fwrite(raw, 1, sizeof raw, f) == sizeof raw;
            fclose(f);
        }
        if (wrote)
            log_line("new identity token written to %s", path);
        else if (path[0])
            log_line("new identity token: the write to %s failed; it is kept for this session only", path);
        else
            log_line("new identity token: no data folder; it is kept for this session only");
    }
    for (i = 0; i < 32; i++)
        snprintf(s_identity + i * 2, 3, "%02x", raw[i]);
    s_identity[64] = 0;

    /* the id: the first 16 bytes of SHA-256(token), hex (lobby.js's deriveId). The server hashes
     * the token's HEX STRING (64 chars), so we hash s_identity, not the raw bytes. CryptoAPI's
     * SHA-256 (the CNG BCryptHash the SDK declares takes a key: this is the plain-hash API) */
    {
        HCRYPTPROV prov = 0;
        HCRYPTHASH h = 0;
        DWORD len = sizeof hash;
        /* PROV_RSA_AES: the PROV_RSA_FULL provider has no SHA-256 (CryptCreateHash fails there) */
        if (CryptAcquireContext(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)
            && CryptCreateHash(prov, CALG_SHA_256, 0, 0, &h)
            && CryptHashData(h, (const BYTE *)s_identity, 64, 0)
            && CryptGetHashParam(h, HP_HASHVAL, hash, &len, 0)) {
            for (i = 0; i < 16; i++)
                snprintf(s_own_id + i * 2, 3, "%02x", hash[i]);
            s_own_id[32] = 0;
        }
        if (h)
            CryptDestroyHash(h);
        if (prov)
            CryptReleaseContext(prov, 0);
    }
    if (s_own_id[0])
        log_line("identity %s", s_own_id);
    else
        log_line("identity: SHA-256 failed (error %lu); this instance can't recognise its own listing",
                 (unsigned long)GetLastError());
}

/* --- guest path: fetch hosts and connect --- */

/* the hosts this fetch listed (to prune s_known: a host that left the list can be joined again) */
static char s_seen[16][33];
static int s_seen_n;

/* the hosts we are connected to, by id (the listing has no endpoints to compare) */
static char s_known[16][33];
static int s_known_n;

/* the connects this worker made, by nonce: the TURN mint needs the host id of a waiting peer */
static char s_conn_id[8][33];
/* The NIC queues under its lock; only the worker sends HTTP. */
#define PROBE_REPORT_QUEUE 16
static char s_report_build[41];
static struct {
    char receipt[33];
    const char *path, *stage;
    uint64_t ms;
} s_probe_reports[PROBE_REPORT_QUEUE];
static unsigned s_report_head, s_report_count;

void fci_net_report_probe(const char *receipt, const char *path, const char *stage, uint64_t started)
{
    const uint64_t ms = GetTickCount64() - started;
    if (!receipt[0] || ms > 120000 || s_report_count == PROBE_REPORT_QUEUE)
        return;
    const unsigned slot = (s_report_head + s_report_count) % PROBE_REPORT_QUEUE;
    memcpy(s_probe_reports[slot].receipt, receipt, sizeof s_probe_reports[slot].receipt);
    s_probe_reports[slot].path = path;
    s_probe_reports[slot].stage = stage;
    s_probe_reports[slot].ms = ms;
    s_report_count++;
}

static void send_probe_report(void)
{
    char body[160], reply[64];
    const char *base = fci_nic_list_url();
    SRWLOCK *lock = fci_nic_lock();
    AcquireSRWLockExclusive(lock);
    if (!base || !s_report_count) {
        ReleaseSRWLockExclusive(lock);
        return;
    }
    const unsigned slot = s_report_head;
    snprintf(body, sizeof body, "{\"receipt\":\"%s\",\"path\":\"%s\",\"stage\":\"%s\",\"ms\":%llu}",
             s_probe_reports[slot].receipt, s_probe_reports[slot].path, s_probe_reports[slot].stage,
             (unsigned long long)s_probe_reports[slot].ms);
    s_report_head = (slot + 1) % PROBE_REPORT_QUEUE;
    s_report_count--;
    ReleaseSRWLockExclusive(lock);
    int status;
    http_request_timed(base, "POST", "/v1/probe-report", body, reply, sizeof reply, &status, 1500);
}

static uint8_t s_conn_nonce[8][16];
static int s_conn_n;

static void on_host_obj(const char *obj, int len)
{
    char id[64], endpoint[128], macstr[32], my_mac[32], my_ep[128];
    char buf[512], resp[256], nhex[33];
    uint8_t mac[6], nonce[16];
    int n, status;
    const char *base;
    SRWLOCK *lock;

    (void)len;
    if (!json_str_val(obj, "id", id, sizeof id))
        return;
    if (strcmp(id, s_own_id) == 0)
        return; /* we are that host (the listing is ids only: no MAC to compare) */
    if (s_seen_n < (int)(sizeof s_seen / sizeof s_seen[0]))
        snprintf(s_seen[s_seen_n++], sizeof s_seen[0], "%s", id);
    for (n = 0; n < s_known_n; n++)
        if (strcmp(s_known[n], id) == 0)
            return; /* already connected to this host */

    base = fci_nic_list_url();
    if (!base)
        return;
    lock = fci_nic_lock();
    AcquireSRWLockExclusive(lock);
    fci_nic_list_endpoint(my_ep, sizeof my_ep);
    ReleaseSRWLockExclusive(lock);
    if (!my_ep[0])
        return; /* no endpoint of our own: nothing to offer */

    fci_nic_mac_str(my_mac, sizeof my_mac);
    gen_nonce(nonce);
    for (n = 0; n < 16; n++)
        snprintf(nhex + n * 2, 3, "%02x", nonce[n]);
    nhex[32] = 0;
    /* POST /v1/connect with the GUEST's own endpoint, MAC, token, and LAN address (if known):
     * the reply carries the host's endpoint and MAC (the listing shows ids only), and the host's
     * LAN address when both sides are behind the same NAT */
    {
        const char *lfield = s_local_addr[0] ? ",\"local\":\"" : "";
        if (s_local_addr[0])
            snprintf(buf, sizeof buf,
                     "{\"id\":\"%s\",\"endpoint\":\"%s\",\"nonce\":\"%s\",\"mac\":\"%s\",\"token\":\"%s\"%s%s\"}",
                     id, my_ep, nhex, my_mac, s_identity, lfield, s_local_addr);
        else
            snprintf(buf, sizeof buf,
                     "{\"id\":\"%s\",\"endpoint\":\"%s\",\"nonce\":\"%s\",\"mac\":\"%s\",\"token\":\"%s\"}",
                     id, my_ep, nhex, my_mac, s_identity);
    }
    if (s_report_build[0]) {
        const size_t length = strlen(buf);
        if (length && buf[length - 1] == '}' && length + strlen(s_report_build) + 18 < sizeof buf)
            snprintf(buf + length - 1, sizeof buf - length + 1, ",\"reportBuild\":\"%s\"}", s_report_build);
    }
    const ULONGLONG report_started = GetTickCount64();
    if (http_request(base, "POST", "/v1/connect", buf, resp, sizeof resp, &status) != 0 || status != 200) {
        log_line("connect to %s failed (status %d)", id, status);
        return;
    }
    char local[64] = {0}, receipt[33] = {0};
    if (s_report_build[0] && json_str_val(resp, "reportReceipt", receipt, sizeof receipt) &&
        (strlen(receipt) != 32 || strspn(receipt, "0123456789abcdef") != 32))
        receipt[0] = 0;
    int ready = json_str_val(resp, "endpoint", endpoint, sizeof endpoint) &&
                json_str_val(resp, "mac", macstr, sizeof macstr) &&
                sscanf(macstr, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &mac[0], &mac[1], &mac[2],
                       &mac[3], &mac[4], &mac[5]) == 6;
    json_str_val(resp, "local", local, sizeof local);
    AcquireSRWLockExclusive(lock);
    if (!ready || !fci_nic_list_add_peer(endpoint, local[0] ? local : NULL, mac, nonce, 1, receipt, report_started))
        fci_net_report_probe(receipt, "unknown", "peer_setup", report_started);
    ReleaseSRWLockExclusive(lock);
    if (!ready)
        return;

    if (s_known_n < (int)(sizeof s_known / sizeof s_known[0]))
        snprintf(s_known[s_known_n++], sizeof s_known[0], "%s", id);
    if (s_conn_n < (int)(sizeof s_conn_id / sizeof s_conn_id[0])) {
        snprintf(s_conn_id[s_conn_n], sizeof s_conn_id[0], "%s", id);
        memcpy(s_conn_nonce[s_conn_n], nonce, 16);
        s_conn_n++;
    }
    log_line("connected to host %s (%s)", id, endpoint);
}

/* --- guest TURN: mint the credential when a punch fails, post the relayed connect when ready --- */

static char s_turn_host_id[33];  /* the host the in-flight mint is for */
static uint8_t s_turn_nonce[16]; /* the nonce the in-flight mint is under (the relayed connect reuses it) */

/* a peer is in LP_TURN_WAIT: POST /v1/turn for its credential and start the allocation. The mint
 * is keyed by host id and nonce (fake_hostlist.py): the relayed connect, with the same nonce and
 * relay:true, is accepted only for a minted guest. */
static void turn_mint(void)
{
    SRWLOCK *lock = fci_nic_lock();
    uint8_t nonce[16];
    char buf[256], resp[1024], hostport[128], user[128], pass[128], nhex[33];
    int n, status, found = 0;
    const char *base;

    AcquireSRWLockExclusive(lock);
    if (!fci_nic_list_turn_wait(nonce)) {
        ReleaseSRWLockExclusive(lock);
        return; /* the peer expired in the meantime: nothing to mint for */
    }
    ReleaseSRWLockExclusive(lock);

    for (n = 0; n < s_conn_n; n++) /* the host id: the connect that seeded this nonce */
        if (!memcmp(s_conn_nonce[n], nonce, 16)) {
            snprintf(s_turn_host_id, sizeof s_turn_host_id, "%s", s_conn_id[n]);
            memcpy(s_turn_nonce, nonce, 16);
            found = 1;
            break;
        }
    if (!found) {
        log_line("turn mint: no host id for the waiting peer's nonce");
        AcquireSRWLockExclusive(lock);
        fci_nic_list_turn_fail();
        ReleaseSRWLockExclusive(lock);
        return;
    }
    for (n = 0; n < 16; n++)
        snprintf(nhex + n * 2, 3, "%02x", nonce[n]);
    nhex[32] = 0;

    base = fci_nic_list_url();
    if (!base)
        return;
    snprintf(buf, sizeof buf, "{\"id\":\"%s\",\"nonce\":\"%s\",\"token\":\"%s\"}", s_turn_host_id, nhex,
             s_identity);
    if (http_request(base, "POST", "/v1/turn", buf, resp, sizeof resp, &status) != 0) {
        log_line("turn mint failed (no response)");
        AcquireSRWLockExclusive(lock);
        fci_nic_list_turn_fail();
        ReleaseSRWLockExclusive(lock);
        return;
    }
    if (status != 200) {
        /* 503 (at capacity) / 403 (banned) / 429 (too many): the NIC falls back to the hub */
        log_line("turn mint refused (status %d), the relay fallback applies", status);
        InterlockedExchange(&s_relay_refused, 1);
        AcquireSRWLockExclusive(lock);
        fci_nic_list_turn_fail();
        ReleaseSRWLockExclusive(lock);
        return;
    }
    if (!json_str_val(resp, "username", user, sizeof user)
        || !json_str_val(resp, "credential", pass, sizeof pass)) {
        log_line("turn mint: a malformed reply");
        AcquireSRWLockExclusive(lock);
        fci_nic_list_turn_fail();
        ReleaseSRWLockExclusive(lock);
        return;
    }
    /* urls[0]: "turn:<host>:<port>?transport=udp" -> "<host>:<port>" */
    {
        /* urls is an array of strings, so json_str (string values only) can't find it */
        const char *u = strstr(resp, "\"urls\""), *url = NULL, *q;
        int len = 0;
        if (u) {
            u += 6;
            while (*u == ' ' || *u == '\t' || *u == ':')
                u++;
            if (*u != '[')
                u = "";
            while (*u && *u != '"')
                u++;
            if (*u == '"')
                url = u + 1;
        }
        if (url) {
            if (strncmp(url, "turn:", 5) == 0)
                url += 5;
            for (q = url; *q && *q != '"' && *q != '?' && len < (int)sizeof hostport - 1; q++)
                hostport[len++] = *q;
            hostport[len] = 0;
        }
        if (!hostport[0]) {
            log_line("turn mint: no server url");
            AcquireSRWLockExclusive(lock);
            fci_nic_list_turn_fail();
            ReleaseSRWLockExclusive(lock);
            return;
        }
    }
    AcquireSRWLockExclusive(lock);
    if (!fci_nic_list_turn_start(hostport, user, pass)) {
        /* the allocation is already in flight, or the waiting peer is gone */
        ReleaseSRWLockExclusive(lock);
        return;
    }
    ReleaseSRWLockExclusive(lock);
    InterlockedExchange(&s_relay_refused, 0); /* a successful mint clears the refusal */
    log_line("turn mint ok (%s)", hostport);
}

/* the allocation is ready and the peer is relayed: POST /v1/connect with the relayed address and
 * relay:true. The host's worker turns its peer live at that address (fci_nic_list_turn_peer). */
static void turn_post_relay(void)
{
    SRWLOCK *lock = fci_nic_lock();
    char ep[64], buf[512], resp[256], macstr[32], nhex[33];
    int n, status;
    const char *base;

    AcquireSRWLockExclusive(lock);
    n = fci_nic_list_turn_poll(ep, sizeof ep);
    ReleaseSRWLockExclusive(lock);
    if (n != 1) {
        log_line("relay connect not posted: the allocation is not ready (%d)", n);
        return;
    }
    base = fci_nic_list_url();
    if (!base) {
        log_line("relay connect not posted: no list url");
        return;
    }
    fci_nic_mac_str(macstr, sizeof macstr);
    for (n = 0; n < 16; n++)
        snprintf(nhex + n * 2, 3, "%02x", s_turn_nonce[n]);
    nhex[32] = 0;
    snprintf(buf, sizeof buf,
             "{\"id\":\"%s\",\"endpoint\":\"%s\",\"nonce\":\"%s\",\"mac\":\"%s\",\"token\":\"%s\","
             "\"relay\":true}",
             s_turn_host_id, ep, nhex, macstr, s_identity);
    if (http_request(base, "POST", "/v1/connect", buf, resp, sizeof resp, &status) != 0 || status != 200) {
        log_line("relay connect to %s failed (status %d)", s_turn_host_id, status);
        return;
    }
    log_line("relay connect posted (%s at %s)", s_turn_host_id, ep);
}

static void guest_fetch(void)
{
    const char *base = fci_nic_list_url();
    char path[256], buf[4096];
    int status, n;

    if (!base)
        return;
    s_last_fetch_attempt = GetTickCount64();
    snprintf(path, sizeof path, "/v1/hosts?title=%s&version=%s&max=12", net_title(), net_version());
    if (http_request(base, "GET", path, NULL, buf, sizeof buf, &status) != 0 || status != 200) {
        log_line("host list fetch failed (status %d), retrying next probe", status);
        InterlockedExchange(&s_fetch_state, 2); /* last fetch failed */
        return;
    }
    log_line("list fetched (%s)", net_title());
    s_seen_n = 0;
    json_array(buf, "hosts", on_host_obj);
    InterlockedExchange(&s_fetch_nhosts, (LONG)s_seen_n);
    if (s_seen_n == 0) {
        fci_nic_browse_clear_dropped();
        InterlockedExchange(&s_relay_refused, 0);
    }
    InterlockedExchange(&s_fetch_state, 1); /* Publish success after the host count. */
    for (n = 0; n < s_known_n; n++) {
        int keep = 0, m;
        for (m = 0; m < s_seen_n; m++)
            if (!strcmp(s_known[n], s_seen[m])) {
                keep = 1;
                break;
            }
        if (!keep) {
            memmove(&s_known[n], &s_known[n + 1], (size_t)(s_known_n - n - 1) * sizeof s_known[0]);
            s_known_n--;
            n--;
        }
    }
}

/* --- host path: register and poll --- */

static void host_register(void)
{
    const char *base = fci_nic_list_url();
    char body[512], buf[512], endpoint[128], macstr[32];
    int status;

    if (!base || s_registered)
        return;

    /* wait for the endpoint (STUN may not have completed) */
    {
        SRWLOCK *lock = fci_nic_lock();
        AcquireSRWLockExclusive(lock);
        int have_ep = fci_nic_list_endpoint(endpoint, sizeof endpoint);
        ReleaseSRWLockExclusive(lock);
        if (!have_ep) {
            /* log once if the endpoint is not known 5 s after host start (with STUN off it never is) */
            if (!s_endpoint_warned && s_host_start_ms && GetTickCount64() - s_host_start_ms >= 5000) {
                s_endpoint_warned = 1;
                log_line("endpoint not known 5 s after host start (STUN off? RECOMP_FCI_NET_ENDPOINT?)");
            }
            return;
        }
    }

    fci_nic_mac_str(macstr, sizeof macstr);

    /* the token is ours (identity_init): the server derives the id from it and returns the id only.
     * The LAN address (s_local_addr) is included when known: the Worker forwards it to a guest
     * only when both sides are behind the same NAT. */
    if (s_local_addr[0])
        snprintf(body, sizeof body,
                 "{\"proto\":%d,\"title\":\"%s\",\"version\":\"%s\",\"mac\":\"%s\",\"endpoint\":\"%s\","
                 "\"local\":\"%s\",\"token\":\"%s\"}",
                 FCI_NET_PROTO, net_title(), net_version(), macstr, endpoint, s_local_addr, s_identity);
    else
        snprintf(body, sizeof body,
                 "{\"proto\":%d,\"title\":\"%s\",\"version\":\"%s\",\"mac\":\"%s\",\"endpoint\":\"%s\","
                 "\"token\":\"%s\"}",
                 FCI_NET_PROTO, net_title(), net_version(), macstr, endpoint, s_identity);
    if (http_request(base, "POST", "/v1/register", body, buf, sizeof buf, &status) != 0 || status != 200) {
        s_reg_fail_ms = GetTickCount64();
        log_line("register failed (status %d)", status);
        return;
    }
    s_reg_fail_ms = 0;
    json_str_val(buf, "id", s_host_id, sizeof s_host_id);
    s_registered = 1;
    log_line("registered as host %s (endpoint %s)", s_host_id, endpoint);
}

static void on_connect_obj(const char *obj, int len)
{
    char endpoint[128], nonce_hex[64], macstr[32], local[64];
    uint8_t mac[6], nonce[16];
    int n, relay;
    (void)len;
    if (!json_str_val(obj, "endpoint", endpoint, sizeof endpoint))
        return;
    if (!json_str_val(obj, "nonce", nonce_hex, sizeof nonce_hex))
        return;
    if (!json_str_val(obj, "mac", macstr, sizeof macstr))
        return;

    if (sscanf(macstr, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4],
               &mac[5])
        != 6)
        return;
    if (strlen(nonce_hex) != 32)
        return;
    for (n = 0; n < 16; n++)
        if (sscanf(nonce_hex + n * 2, "%2hhx", &nonce[n]) != 1)
            return;

    relay = json_bool(obj, "relay");
    if (relay < 0)
        relay = 0;

    /* the guest's LAN address (when the Worker forwarded one: both sides are behind the same NAT) */
    json_str_val(obj, "local", local, sizeof local);

    {
        SRWLOCK *lock = fci_nic_lock();
        AcquireSRWLockExclusive(lock);
        if (relay) {
            /* the guest's relayed connect: its peer is live at the relayed address (TURN) */
            fci_nic_list_turn_peer(nonce, mac, endpoint);
        } else {
            fci_nic_list_add_peer(endpoint, local[0] ? local : NULL, mac, nonce, 0, NULL, 0); /* a guest joined us */
        }
        ReleaseSRWLockExclusive(lock);
    }
    if (relay)
        log_line("connect received from %s (relay, nonce %.8s)", endpoint, nonce_hex);
    else
        log_line("connect received from %s", endpoint);
}

static void host_poll(void)
{
    const char *base = fci_nic_list_url();
    char path[256], buf[4096];
    int status;

    if (!base || !s_registered)
        return;
    snprintf(path, sizeof path, "/v1/connects?id=%s&token=%s&wait=%d", s_host_id, s_identity, POLL_WAIT_S);
    if (http_request(base, "GET", path, NULL, buf, sizeof buf, &status) != 0) {
        log_line("connect poll failed, backing off 5 s");
        Sleep(5000);
        return;
    }
    if (status == 404) {
        /* the registration expired: re-register once */
        log_line("connect poll 404: re-registering");
        s_registered = 0;
        host_register();
        return;
    }
    if (status == 200) {
        json_array(buf, "connects", on_connect_obj);
    }
    /* 204: no connects, the poll timed out; loop */
}

/* Leave the host list on any clean exit: without it the listing stays joinable until its 90-second
 * TTL (lobby.js) and the next instance connects to a dead host. Idempotent and safe from any thread
 * (the window thread on WM_CLOSE, the game thread on a quit-to-dashboard, the worker on stop): the
 * leave happens once -- the InterlockedExchange hands it to a single caller -- and is bounded to
 * about 1.5 s (http_request_timed), so no exit is delayed by more than that. It takes no NIC lock:
 * fci_nic_list_url is lock-free, and the id and token are written once at registration and never
 * changed. research/docs/online-architecture.md, "Leaving the list". */
void fci_net_leave(void)
{
    const char *base;
    char body[256], resp[128];
    int status;

    if (InterlockedExchange((volatile LONG *)&s_registered, 0) == 0)
        return; /* not registered, or already left: nothing to do */
    base = fci_nic_list_url();
    if (!base)
        return;
    snprintf(body, sizeof body, "{\"id\":\"%s\",\"token\":\"%s\"}", s_host_id, s_identity);
    if (http_request_timed(base, "POST", "/v1/unregister", body, resp, sizeof resp, &status, 1500) != 0)
        log_line("unregister: no response (1.5 s budget)");
    else
        log_line("unregistered (status %d)", status);
}

/* --- thread function --- */

static DWORD WINAPI worker_main(LPVOID arg)
{
    (void)arg;
    s_session = WinHttpOpen(L"fci-recomp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s_session) {
        log_line("WinHttpOpen failed (%u)", (unsigned)GetLastError());
        return 1;
    }
    /* set a timeout: 25 s for the long-poll (20 s wait + 5 s margin) */
    {
        DWORD t = 25000;
        WinHttpSetOption(s_session, WINHTTP_OPTION_SEND_TIMEOUT, &t, sizeof t);
        WinHttpSetOption(s_session, WINHTTP_OPTION_RECEIVE_TIMEOUT, &t, sizeof t);
    }

    local_addr_detect(); /* the card's LAN address (RECOMP_FCI_NET_LOCAL or auto-detect) */

    while (WaitForSingleObject(s_stop, WORKER_TICK_MS) == WAIT_TIMEOUT) {
        if (s_host_started && !s_registered) {
            if (!s_host_start_ms)
                s_host_start_ms = GetTickCount64();
            if (!s_reg_fail_ms || GetTickCount64() - s_reg_fail_ms >= 5000)
                host_register();
        }
        if (s_registered)
            host_poll();
        int probe = fci_nic_worker_probe_take();
        if (probe == FCI_LIST_PROBE_ENTRY ||
            (probe == FCI_LIST_PROBE_DISCOVERY &&
             (!s_last_fetch_attempt || GetTickCount64() - s_last_fetch_attempt >= LIST_FETCH_INTERVAL_MS)))
            guest_fetch();
        /* the TURN relay: a punch failed (mint), or the allocation is ready (post the relayed connect) */
        if (fci_nic_list_turn_need_take())
            turn_mint();
        if (fci_nic_list_turn_ready_take())
            turn_post_relay();
        if (s_report_build[0])
            send_probe_report();
    }

    if (s_session) {
        WinHttpCloseHandle(s_session);
        s_session = NULL;
    }
    return 0;
}

/* --- public API --- */

/* the worker's contribution to the browse status (Q29 round 4): fetch_state, n_hosts, relay_refused,
 * version_refused. Called from the game thread (the relabel gate); reads volatile state set by the
 * worker thread. */
void fci_net_worker_browse_state(int *fetch_state, int *n_hosts, int *relay_refused, int *version_refused)
{
    *fetch_state = (int)s_fetch_state;
    *n_hosts = (int)s_fetch_nhosts;
    *relay_refused = (int)s_relay_refused;
    *version_refused = (int)s_version_refused;
}

void fci_net_worker_start(void)
{
    s_report_build[0] = 0;
#ifdef FCI_BETA_REPORTS
    const char *opt_in = getenv("RECOMP_FCI_BETA_REPORT");
    const char *build = getenv("RECOMP_FCI_BETA_BUILD");
    const char *alnum = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const char *build_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-";
    if (opt_in && !strcmp(opt_in, "1") && build && build[0] && strchr(alnum, build[0]) &&
        strlen(build) < sizeof s_report_build && strspn(build, build_chars) == strlen(build))
        strcpy(s_report_build, build);
#endif
    s_report_head = s_report_count = 0;
    identity_init();
    s_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_thread = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    if (!s_thread) {
        log_line("worker thread creation failed (%u)", (unsigned)GetLastError());
        CloseHandle(s_stop);
        s_stop = NULL;
    }
}

void fci_net_worker_stop(void)
{
    if (s_stop) {
        /* leave the host list before the session closes: the next instance must not find a dead
         * host. Runs on this (the game's) thread, on its own session, so the worker thread's
         * pending long-poll is untouched (research/docs/online-architecture.md, "Leaving the list") */
        fci_net_leave();
        /* close the session first: WinHttpCloseHandle cancels pending requests, so the
         * thread unblocks from a long-poll immediately; guard the handle with the lock */
        if (s_session) {
            WinHttpCloseHandle(s_session);
            s_session = NULL;
        }
        SetEvent(s_stop);
        if (s_thread) {
            WaitForSingleObject(s_thread, 5000);
            CloseHandle(s_thread);
        }
        CloseHandle(s_stop);
        s_stop = NULL;
        s_thread = NULL;
    }
}

void fci_prehook_0051C5EA(void)
{
    /* XNetCreateKey: the game is creating a host's key. Set the flag; the worker picks it up. */
    InterlockedExchange(&s_host_started, 1);
}

/* The network worker thread: one WinHTTP session for the host list (RECOMP_FCI_NET_LIST).
 * Guest path: the NIC signals "probe" on broadcast; the worker fetches /v1/hosts, adds peers,
 * and posts /v1/connect for each new host.
 * Host path: fci_prehook_0051C5EA (XNetCreateKey: only a host creates a key) sets "host started"; the worker registers
 * and long-polls /v1/connects.
 * It never touches the card's UDP socket; it takes s_lock only around fci_nic_list_* calls.
 * research/docs/multiplayer.md, tools/fake_hostlist.py */
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fci_image.h"
#include "fci_net_worker.h"

/* the NIC's worker API (fci_nic.c): called under s_lock */
extern int fci_nic_list_add_peer(const char *endpoint, const uint8_t mac[6], const uint8_t nonce[16]);
extern int fci_nic_list_endpoint(char *buf, int buflen);
extern void fci_nic_mac(uint8_t *mac);
extern void fci_nic_mac_str(char *buf, int buflen);
extern int fci_nic_list_has_peer(const char *endpoint);
extern int fci_nic_worker_probe_take(void);
extern const char *fci_nic_list_url(void);
extern SRWLOCK *fci_nic_lock(void); /* the NIC's s_lock */

/* the XBE certificate's Version field (fci_main.c) */
extern uint32_t fci_xbe_version;

#define LIST_FETCH_INTERVAL_MS 5000
#define POLL_WAIT_S            20
#define WORKER_TICK_MS         500
#define NET_PROTO              1 /* the host list protocol version (lobby.js, fake_hostlist.py) */

static HANDLE s_thread;
static HANDLE s_stop;
static HINTERNET s_session;
static volatile LONG s_host_started; /* set by fci_prehook_0051C5EA */

/* host registration state */
static char s_host_id[64];
static char s_host_token[64];
static int s_registered;
static ULONGLONG s_last_fetch;    /* last /v1/hosts fetch (GetTickCount64) */
static ULONGLONG s_host_start_ms; /* when "host started" was set (for the endpoint-wait log) */
static int s_endpoint_warned;     /* logged once if the endpoint is not known 5 s after host start */

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

/* do a request; returns the response body in buf (up to buflen), 0 on success, -1 on error.
 * method: "GET" or "POST"; path: the URL path+query; body: POST body or NULL.
 * Returns the HTTP status code in *status. */
static int http_request(const char *base_url, const char *method, const char *path, const char *body,
                        char *buf, int buflen, int *status)
{
    /* parse base_url into scheme, host, port */
    char host[256];
    int port = 80;
    int secure = 0;
    const char *p = base_url;
    wchar_t wmethod[16], wpath[512], whost[256], wheaders[64];
    HINTERNET hconnect = NULL;
    HINTERNET hrequest = NULL;
    DWORD bytes = 0, total = 0;

    if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        port = 443;
        secure = 1;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else {
        return -1;
    }
    /* find the end of host:port */
    {
        const char *slash = strchr(p, '/');
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen >= sizeof host)
            hlen = sizeof host - 1;
        memcpy(host, p, hlen);
        host[hlen] = 0;
        {
            char *colon = strrchr(host, ':');
            if (colon && strchr(colon + 1, '/') == NULL) {
                *colon = 0;
                port = atoi(colon + 1);
            }
        }
    }

    to_wide(method, wmethod, 16);
    to_wide(path, wpath, 512);
    to_wide(host, whost, 256);
    wcscpy(wheaders, L"Content-Type: application/json\r\n"); /* the Worker answers 415 to anything else */

    hconnect = WinHttpConnect(s_session, whost, (UINT)port, 0);
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

/* --- nonce generation --- */

static void gen_nonce(uint8_t nonce[16])
{ BCryptGenRandom(NULL, nonce, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG); }

/* --- guest path: fetch hosts and connect --- */

static void on_host_obj(const char *obj, int len)
{
    char id[64], endpoint[128], macstr[32], my_mac[32], my_ep[128];
    char buf[512], nhex[33];
    uint8_t mac[6], nonce[16];
    int n;

    (void)len;
    if (!json_str_val(obj, "id", id, sizeof id))
        return;
    if (!json_str_val(obj, "endpoint", endpoint, sizeof endpoint))
        return;
    if (!json_str_val(obj, "mac", macstr, sizeof macstr))
        return;

    /* parse the host's MAC */
    if (sscanf(macstr, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4],
               &mac[5])
        != 6)
        return;

    /* skip our own MAC (we are the host, not a guest of ourselves) */
    fci_nic_mac_str(my_mac, sizeof my_mac);
    if (strcmp(my_mac, macstr) == 0)
        return;

    /* skip if the NIC already has this peer (pending or live) */
    {
        SRWLOCK *lock = fci_nic_lock();
        AcquireSRWLockExclusive(lock);
        int have = fci_nic_list_has_peer(endpoint);
        ReleaseSRWLockExclusive(lock);
        if (have)
            return;
    }

    gen_nonce(nonce);

    /* add the peer (takes s_lock) */
    {
        SRWLOCK *lock = fci_nic_lock();
        AcquireSRWLockExclusive(lock);
        fci_nic_list_add_peer(endpoint, mac, nonce);
        ReleaseSRWLockExclusive(lock);
    }

    /* POST /v1/connect with the GUEST's own endpoint and MAC */
    {
        const char *base = fci_nic_list_url();
        if (!base)
            return;
        {
            SRWLOCK *lock = fci_nic_lock();
            AcquireSRWLockExclusive(lock);
            fci_nic_list_endpoint(my_ep, sizeof my_ep);
            ReleaseSRWLockExclusive(lock);
        }
        for (n = 0; n < 16; n++)
            sprintf(nhex + n * 2, "%02x", nonce[n]);
        nhex[32] = 0;
        snprintf(buf, sizeof buf, "{\"id\":\"%s\",\"endpoint\":\"%s\",\"nonce\":\"%s\",\"mac\":\"%s\"}", id,
                 my_ep, nhex, my_mac);
        int status;
        char resp[64];
        http_request(base, "POST", "/v1/connect", buf, resp, sizeof resp, &status);
        log_line("connect posted to %s (%s)", id, status == 204 ? "ok" : "failed");
    }
}

static void guest_fetch(void)
{
    const char *base = fci_nic_list_url();
    char path[256], buf[4096];
    int status;

    if (!base)
        return;
    snprintf(path, sizeof path, "/v1/hosts?title=%s&version=%s&max=12", net_title(), net_version());
    if (http_request(base, "GET", path, NULL, buf, sizeof buf, &status) != 0 || status != 200) {
        log_line("host list fetch failed (status %d), retrying next probe", status);
        return;
    }
    s_last_fetch = GetTickCount64();
    log_line("list fetched (%s)", net_title());
    json_array(buf, "hosts", on_host_obj);
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

    snprintf(body, sizeof body,
             "{\"proto\":%d,\"title\":\"%s\",\"version\":\"%s\",\"mac\":\"%s\",\"endpoint\":\"%s\"}",
             NET_PROTO, net_title(), net_version(), macstr, endpoint);
    if (http_request(base, "POST", "/v1/register", body, buf, sizeof buf, &status) != 0 || status != 200) {
        log_line("register failed (status %d)", status);
        return;
    }
    json_str_val(buf, "id", s_host_id, sizeof s_host_id);
    json_str_val(buf, "token", s_host_token, sizeof s_host_token);
    s_registered = 1;
    log_line("registered as host %s (endpoint %s)", s_host_id, endpoint);
}

static void on_connect_obj(const char *obj, int len)
{
    char endpoint[128], nonce_hex[64], macstr[32];
    uint8_t mac[6], nonce[16];
    int n;

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

    {
        SRWLOCK *lock = fci_nic_lock();
        AcquireSRWLockExclusive(lock);
        fci_nic_list_add_peer(endpoint, mac, nonce);
        ReleaseSRWLockExclusive(lock);
    }
    log_line("connect received from %s", endpoint);
}

static void host_poll(void)
{
    const char *base = fci_nic_list_url();
    char path[256], buf[4096];
    int status;

    if (!base || !s_registered)
        return;
    snprintf(path, sizeof path, "/v1/connects?id=%s&token=%s&wait=%d", s_host_id, s_host_token, POLL_WAIT_S);
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

    while (WaitForSingleObject(s_stop, WORKER_TICK_MS) == WAIT_TIMEOUT) {
        if (s_host_started && !s_registered) {
            if (!s_host_start_ms)
                s_host_start_ms = GetTickCount64();
            host_register();
        }
        if (s_registered)
            host_poll();
        if (fci_nic_worker_probe_take() && GetTickCount64() - s_last_fetch >= LIST_FETCH_INTERVAL_MS)
            guest_fetch();
        /* if the probe is set but the list is fresh, clear it (the list is current) */
        fci_nic_worker_probe_take();
    }

    if (s_session) {
        WinHttpCloseHandle(s_session);
        s_session = NULL;
    }
    return 0;
}

/* --- public API --- */

void fci_net_worker_start(void)
{
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

/* The host list's JSON arrays of flat objects: {"hosts":[{...},{...}]}, {"connects":[...]}, and the
 * field readers the Worker uses to pull a value out of one object. No sockets or globals, so a test
 * (tests/json_test.c) runs this exact code natively. */
#include "fci_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fci_json_array(const char *buf, const char *key, void (*fn)(const char *obj, int len))
{
    char pat[64], obj[FCI_JSON_OBJ_MAX];
    const char *p;
    int skipped = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(buf, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    while (*p && *p != '[')
        p++;
    if (*p != '[')
        return -1;
    p++;
    for (;;) {
        const char *start;
        int depth = 0;
        while (*p && *p != '{' && *p != ']')
            p++;
        if (*p != '{')
            break; /* the array's end, or a cut-off reply */
        start = p;
        while (*p) { /* to the matching } (p ends just past it) */
            if (*p == '{')
                depth++;
            else if (*p == '}' && --depth == 0) {
                p++;
                break;
            }
            p++;
        }
        if (depth != 0)
            break; /* an object cut off by the end of the buffer: not handed on */
        if (p - start >= (int)sizeof obj) {
            skipped++;
            continue;
        }
        memcpy(obj, start, (size_t)(p - start));
        obj[p - start] = 0;
        fn(obj, (int)(p - start));
    }
    return skipped;
}

/* find "key" in buf, return a pointer to the value (after the colon and any whitespace) */
const char *fci_json_str(const char *buf, const char *key)
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
int fci_json_str_val(const char *buf, const char *key, char *out, int buflen)
{
    const char *p = fci_json_str(buf, key);
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
int fci_json_bool(const char *buf, const char *key)
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

/* extract a non-negative integer value; 0 when absent */
unsigned long long fci_json_u64(const char *buf, const char *key)
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(buf, pat);
    if (!p)
        return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p < '0' || *p > '9') /* strtoull takes "-5" as a huge number: a seq that would skip every connect */
        return 0;
    return strtoull(p, NULL, 10);
}

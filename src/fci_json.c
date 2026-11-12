/* The host list's JSON arrays of flat objects: {"hosts":[{...},{...}]}, {"connects":[...]}. */
#include "fci_json.h"

#include <stdio.h>
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

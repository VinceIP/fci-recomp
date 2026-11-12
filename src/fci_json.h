/* The host list's JSON arrays of flat objects (src/fci_json.c); no sockets or globals, so a test runs it. */
#ifndef FCI_JSON_H
#define FCI_JSON_H

/* the longest object fci_json_array hands on; a longer one is skipped (and counted in the return) */
#define FCI_JSON_OBJ_MAX 1024

/* Calls fn(obj, len) for each {...} in the array under "key", in order. obj is a NUL-terminated copy of
 * that object alone, so a field lookup in it cannot run on into the next object. Returns the number of
 * objects skipped as too long, or -1 when there is no such array. */
int fci_json_array(const char *buf, const char *key, void (*fn)(const char *obj, int len));

#endif

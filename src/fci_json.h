/* The host list's JSON arrays of flat objects (src/fci_json.c); no sockets or globals, so a test runs it. */
#ifndef FCI_JSON_H
#define FCI_JSON_H

/* the longest object fci_json_array hands on; a longer one is skipped (and counted in the return) */
#define FCI_JSON_OBJ_MAX 1024

/* Calls fn(obj, len) for each {...} in the array under "key", in order. obj is a NUL-terminated copy of
 * that object alone, so a field lookup in it cannot run on into the next object. Returns the number of
 * objects skipped as too long, or -1 when there is no such array. */
int fci_json_array(const char *buf, const char *key, void (*fn)(const char *obj, int len));

/* --- minimal JSON field readers (the Worker's host-list replies; flat objects, no nesting) ---
 * Each finds "key" in buf (a NUL-terminated object) and returns the value, or a sentinel when it
 * is absent or not the expected type. They are the Worker's field extraction, moved here so a test
 * runs this exact code natively; src/fci_net_worker.c calls them. */

/* find "key" in buf, return a pointer to the string value (after the colon and any whitespace), or
 * NULL when the key is absent, has no colon, or its value is not a quoted string. */
const char *fci_json_str(const char *buf, const char *key);

/* extract a string value: copies up to buflen-1 chars until the closing quote. 1 when found and
 * non-empty, 0 when the key is absent or its value is empty. */
int fci_json_str_val(const char *buf, const char *key, char *out, int buflen);

/* extract a boolean value: 1 for true, 0 for false, -1 when absent or not a boolean. */
int fci_json_bool(const char *buf, const char *key);

/* extract a non-negative integer value; 0 when absent or not a number. */
unsigned long long fci_json_u64(const char *buf, const char *key);

#endif

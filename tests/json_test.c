/* Driver for tests/test_json_c.py: runs src/fci_json.c on host-list replies and prints one line a check. */
#include <stdio.h>
#include <string.h>

#include "fci_json.h"

static char seen[16][FCI_JSON_OBJ_MAX];
static int nseen;
static int failed, checks;

static void collect(const char *obj, int len)
{
    if (nseen < 16 && (int)strlen(obj) == len)
        snprintf(seen[nseen++], sizeof seen[0], "%s", obj);
}

static int run(const char *buf, const char *key)
{
    nseen = 0;
    return fci_json_array(buf, key, collect);
}

static void check(const char *name, int ok)
{
    checks++;
    failed += !ok;
    printf("%s %s:\n", ok ? "ok  " : "FAIL", name);
}

int main(void)
{
    static char big[4096];
    int r;

    /* the Worker's form (JSON.stringify): no spaces; three hosts, all three in order */
    r = run("{\"proto\":2,\"hosts\":[{\"id\":\"aa\"},{\"id\":\"bb\"},{\"id\":\"cc\"}]}", "hosts");
    check("three-hosts", r == 0 && nseen == 3 && !strcmp(seen[0], "{\"id\":\"aa\"}")
                                 && !strcmp(seen[1], "{\"id\":\"bb\"}")
                                 && !strcmp(seen[2], "{\"id\":\"cc\"}"));

    /* Python's json.dumps form (the fake list): ", " and ": " */
    r = run("{\"proto\": 2, \"hosts\": [{\"id\": \"aa\"}, {\"id\": \"bb\"}]}", "hosts");
    check("spaced-two-hosts", r == 0 && nseen == 2 && strstr(seen[1], "bb") != NULL);

    /* a direct connect then a relayed one: the first object alone, so "relay" is not found in it */
    r = run("{\"connects\":[{\"endpoint\":\"1.2.3.4:5\",\"nonce\":\"n1\",\"mac\":\"m\"},"
            "{\"endpoint\":\"6.7.8.9:10\",\"nonce\":\"n1\",\"mac\":\"m\",\"relay\":true}]}",
            "connects");
    check("connects-bounded", r == 0 && nseen == 2 && strstr(seen[0], "relay") == NULL
                                      && strstr(seen[1], "\"relay\":true") != NULL);

    r = run("{\"proto\":2,\"hosts\":[]}", "hosts");
    check("empty", r == 0 && nseen == 0);

    r = run("{\"proto\":2}", "hosts");
    check("no-array", r == -1 && nseen == 0);

    /* an over-long object is skipped and counted; its neighbours still arrive */
    memset(big, 0, sizeof big);
    strcpy(big, "{\"hosts\":[{\"id\":\"aa\"},{\"id\":\"");
    memset(big + strlen(big), 'x', FCI_JSON_OBJ_MAX + 10);
    strcat(big, "\"},{\"id\":\"cc\"}]}");
    r = run(big, "hosts");
    check("too-long-skipped", r == 1 && nseen == 2 && strstr(seen[1], "cc") != NULL);

    /* a reply cut off inside the second object: only the whole first one */
    r = run("{\"hosts\":[{\"id\":\"aa\"},{\"id\":\"b", "hosts");
    check("cut-off", r == 0 && nseen == 1);

    printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
}

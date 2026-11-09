"""The host list operator CLI (the /admin routes of tools/hostlist/lobby.js).

Stdlib only. The base URL and the admin secret come from the command line or the environment
($FCI_HOSTLIST_URL, $FCI_HOSTLIST_ADMIN_SECRET); the secret is what wrangler put in the Worker as
ADMIN_SECRET.

Usage:
  python tools/hostlist/admin.py hosts                 # the live hosts (ids, addresses, pending)
  python tools/hostlist/admin.py bans                  # the bans (persistent)
  python tools/hostlist/admin.py ban <id> [reason ...] # ban an id (it cannot host or join)
  python tools/hostlist/admin.py unban <id>
  python tools/hostlist/admin.py abuse                 # the refusal counters and the recent ring

The ids are the 32-hex player ids the runtime derives from its identity token; the `abuse` ring
shows which ids an address was refused for, which is how you find the id to ban.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("command", choices=["hosts", "bans", "ban", "unban", "abuse"])
    ap.add_argument("id", nargs="?", help="the 32-hex id (ban, unban)")
    ap.add_argument("reason", nargs="*", help="the ban reason (optional)")
    ap.add_argument(
        "--url",
        default=os.environ.get("FCI_HOSTLIST_URL"),
        help="the Worker base URL (default $FCI_HOSTLIST_URL)",
    )
    ap.add_argument(
        "--secret",
        default=os.environ.get("FCI_HOSTLIST_ADMIN_SECRET"),
        help="the admin secret (default $FCI_HOSTLIST_ADMIN_SECRET)",
    )
    args = ap.parse_args()

    if not args.url:
        sys.exit("no base URL: --url or $FCI_HOSTLIST_URL")
    if not args.secret:
        sys.exit("no admin secret: --secret or $FCI_HOSTLIST_ADMIN_SECRET")

    method = "GET"
    path = {
        "hosts": "/admin/hosts",
        "bans": "/admin/bans",
        "abuse": "/admin/abuse",
        "ban": "/admin/bans",
        "unban": f"/admin/bans?id={args.id}",
    }[args.command]
    body = None
    if args.command == "ban":
        if not args.id or not len(args.id) == 32:
            sys.exit("ban needs the 32-hex id")
        method = "POST"
        body = json.dumps({"id": args.id, "reason": " ".join(args.reason)}).encode()
    if args.command == "unban":
        if not args.id or not len(args.id) == 32:
            sys.exit("unban needs the 32-hex id")
        method = "DELETE"

    req = urllib.request.Request(args.url.rstrip("/") + path, data=body, method=method)
    req.add_header("authorization", "Bearer " + args.secret)
    if body:
        req.add_header("content-type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read())
    except urllib.error.HTTPError as e:
        sys.exit(f"HTTP {e.code}: {e.read().decode()}")

    if args.command == "hosts":
        for h in data["hosts"]:
            print(f"{h['id']}  {h['ip']:<15} {h['title']}/{h['version']}  {h['endpoint']}  pending={h['pending']}")
        if not data["hosts"]:
            print("(no live hosts)")
    elif args.command == "bans":
        for b in data["bans"]:
            print(f"{b['id']}  {b['date']}  {b['reason']}")
        if not data["bans"]:
            print("(no bans)")
    elif args.command == "ban":
        print("banned" if data.get("ok") else data)
    elif args.command == "unban":
        print("removed" if data.get("removed") else "not banned")
    elif args.command == "abuse":
        print("by address (total refusals):")
        for c in data["byIp"]:
            print(f"  {c['ip']:<15} rate={c['rate']} bad={c['bad']} nohost={c['nohost']} banned={c['banned']}")
        print("recent refusals (newest last):")
        for e in data["ring"][-50:]:
            print(f"  {e['at']}  {e['ip']:<15} {e['kind']:<7} {e['id'] or ''}")


if __name__ == "__main__":
    main()

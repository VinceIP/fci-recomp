"""Collect a private beta monitoring snapshot as JSON."""

import argparse
from datetime import UTC, datetime, timedelta
import json
import math
import os
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

GRAPHQL = "https://api.cloudflare.com/client/v4/graphql"


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def request_json(url, credential, payload=None):
    headers = {"Authorization": f"Bearer {credential}", "User-Agent": "fci-hostlist-monitor/1"}
    if payload is not None:
        headers["Content-Type"] = "application/json"
    data = json.dumps(payload).encode() if payload is not None else None
    try:
        with build_opener(NoRedirect()).open(Request(url, data=data, headers=headers), timeout=20) as response:
            return json.load(response)
    except HTTPError as error:
        raise ValueError(f"HTTP {error.code}") from None
    except (URLError, OSError, ValueError):
        raise ValueError("request or JSON read failed") from None


def analytics(dataset, fields, scope, name, account, credential, start, end):
    query = (
        "query Usage($account: string!, $name: string!, $start: Time!, $end: Time!) {"
        " viewer { accounts(filter: {accountTag: $account}) {"
        f" {dataset}(limit: 1, filter: {{{scope}: $name, datetime_geq: $start, datetime_lt: $end}})"
        f" {{ sum {{ {' '.join(fields)} }} }} }} }} }}"
    )
    result = request_json(
        GRAPHQL,
        credential,
        {
            "query": query,
            "variables": {
                "account": account,
                "name": name,
                "start": start,
                "end": end,
            },
        },
    )
    if not isinstance(result, dict) or result.get("errors"):
        raise ValueError("Cloudflare rejected the analytics query")
    try:
        rows = result["data"]["viewer"]["accounts"][0][dataset]
        totals = {field: 0 for field in fields} if rows == [] else rows[0]["sum"]
        values = {field: totals[field] for field in fields}
    except (KeyError, IndexError, TypeError):
        raise ValueError("missing analytics data") from None
    if any(type(n) not in (int, float) or not math.isfinite(n) or n < 0 for n in values.values()):
        raise ValueError("invalid analytics total")
    return values


def collect(args, env, now):
    end = now.replace(microsecond=0)
    start = end - timedelta(hours=args.hours)
    snapshot = {"from": start.isoformat(), "to": end.isoformat(), "errors": {}}

    def read(name, fn, *params):
        try:
            snapshot[name] = fn(*params)
        except ValueError as error:
            snapshot["errors"][name] = str(error)

    account, credential = env.get("CLOUDFLARE_ACCOUNT_ID"), env.get("CLOUDFLARE_API_TOKEN")
    if account and credential:
        read(
            "workers",
            analytics,
            "workersInvocationsAdaptive",
            ("requests", "errors"),
            "scriptName",
            args.script,
            account,
            credential,
            snapshot["from"],
            snapshot["to"],
        )
        if args.namespace:
            read(
                "durableObject",
                analytics,
                "durableObjectsPeriodicGroups",
                ("duration",),
                "namespaceId",
                args.namespace,
                account,
                credential,
                snapshot["from"],
                snapshot["to"],
            )
    else:
        snapshot["errors"]["cloudflare"] = "analytics environment is incomplete"

    if args.url:
        authorization = env.get("FCI_HOSTLIST_ADMIN_SECRET")
        url = urlsplit(args.url)
        if not authorization:
            snapshot["errors"]["hostlist"] = "admin environment is incomplete"
        elif (
            not url.hostname
            or url.username
            or url.password
            or url.query
            or url.fragment
            or (
                url.scheme != "https"
                and not (url.scheme == "http" and url.hostname in {"127.0.0.1", "::1", "localhost"})
            )
        ):
            snapshot["errors"]["hostlist"] = "use an HTTPS host-list URL or a loopback HTTP URL"
        else:
            read("hostlist", request_json, args.url.rstrip("/") + "/admin/metrics", authorization)
            read("relay", request_json, args.url.rstrip("/") + "/admin/relay", authorization)
    return snapshot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hours", type=int, choices=range(1, 25), default=24, metavar="1-24")
    parser.add_argument("--script", default="fci-hostlist")
    parser.add_argument("--namespace", default=os.environ.get("FCI_DO_NAMESPACE_ID"))
    parser.add_argument("--url", default=os.environ.get("FCI_HOSTLIST_URL"))
    args = parser.parse_args()
    snapshot = collect(args, os.environ, datetime.now(UTC))
    print(json.dumps(snapshot, indent=2, sort_keys=True))
    return bool(snapshot["errors"])


if __name__ == "__main__":
    raise SystemExit(main())

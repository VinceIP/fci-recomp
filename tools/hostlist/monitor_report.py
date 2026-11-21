"""Render a private monitoring snapshot without loading external resources."""

from datetime import UTC, datetime
from html import escape


def number(value):
    return f"{value:,.2f}".rstrip("0").rstrip(".") if type(value) in (int, float) else "—"


def time_label(ms):
    if type(ms) not in (int, float):
        return "Unavailable"
    return datetime.fromtimestamp(ms / 1000, UTC).strftime("%Y-%m-%d %H:%M:%S UTC")


def table(headings, rows, empty):
    if not rows:
        return f'<p class="note">{escape(empty)}</p>'
    head = "".join(f'<th scope="col">{escape(label)}</th>' for label in headings)
    body = "".join("<tr>" + "".join(f"<td>{escape(str(value))}</td>" for value in row) + "</tr>" for row in rows)
    return f'<div class="scroll"><table><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table></div>'


def route_rows(routes, before):
    rows = []
    for route, row in sorted(routes.items()):
        old = before.get(route, {})
        count = row["requests"] - old.get("requests", 0)
        elapsed = row["totalMs"] - old.get("totalMs", 0)
        statuses = {key: value - old.get("status", {}).get(key, 0) for key, value in sorted(row["status"].items())}
        errors = sum(value for key, value in statuses.items() if int(key) >= 400)
        rows.append(
            (
                route,
                number(count),
                number(errors),
                f"{errors / count:.1%}" if count else "—",
                number(elapsed / count) if count else "—",
                number(row["maxMs"]),
                " · ".join(f"{key} × {value}" for key, value in statuses.items() if value) or "—",
            )
        )
    return rows


def render(snapshot, baseline=None):
    workers = snapshot.get("workers", {})
    relay = snapshot.get("relay", {})
    hostlist = snapshot.get("hostlist", {})
    started = hostlist.get("startedAt")
    before = (baseline or {}).get("hostlist", {})
    comparable = (
        started is not None
        and started == before.get("startedAt")
        and hostlist.get("sampledAt", 0) >= before.get("sampledAt", 0)
    )
    if not comparable:
        before = {}
    window = "Since baseline" if comparable else "Since lobby start"
    warning = ""
    if baseline is not None and not comparable:
        warning = (
            '<p class="notice">Baseline is from a different lobby instance or a later sample. '
            "Showing current totals.</p>"
        )

    active = hostlist.get("active", {})
    used, budget = relay.get("usedBytes"), relay.get("budgetBytes")
    used_gb = used / 1e9 if used is not None else None
    budget_gb = budget / 1e9 if budget is not None else None
    remaining = max(0, budget - used) / 1e9 if budget is not None and used is not None else None
    relay_status = {True: "Mints allowed", False: "Mints blocked"}.get(relay.get("allow"), "Unavailable")
    cards = (
        ("Active listings", number(sum(active.values())) if "active" in hostlist else "—", "Current lobby"),
        ("Worker requests", number(workers.get("requests")), "Selected Cloudflare window"),
        ("Invocation errors", number(workers.get("errors")), "Selected Cloudflare window"),
        ("Durable Object time", number(snapshot.get("durableObject", {}).get("duration")), "GB-seconds in window"),
    )
    cards_html = "".join(
        f'<div class="card"><span>{escape(label)}</span><strong>{value}</strong><small>{escape(detail)}</small></div>'
        for label, value, detail in cards
    )
    problem = snapshot.get("errors", {})
    if problem:
        warning += (
            '<ul class="notice">'
            + "".join(f"<li>{escape(str(name))} · {escape(str(error))}</li>" for name, error in sorted(problem.items()))
            + "</ul>"
        )

    routes = table(
        ("Route", "Requests", "HTTP 4xx/5xx", "Error rate", "Avg ms", "Lifetime max ms", "HTTP status counts"),
        route_rows(hostlist.get("routes", {}), before.get("routes", {})),
        "No recorded routes in this snapshot.",
    )
    listings = table(
        ("Game / protocol", "Listings"),
        [(key, number(count)) for key, count in sorted(active.items())],
        "No active listings." if "active" in hostlist else "Listing data unavailable.",
    )
    outcomes = []
    for group, labels in (
        (
            "connects",
            {
                "directQueued": "Direct queued",
                "directDelivered": "Direct delivered",
                "relayQueued": "Relay queued",
                "relayDelivered": "Relay delivered",
            },
        ),
        (
            "relay",
            {
                "issued": "Credentials issued",
                "unconfigured": "Relay unconfigured",
                "budget": "Budget refusal",
                "disabled": "Relay disabled",
                "usageUnavailable": "Usage unavailable",
                "providerError": "Provider error",
            },
        ),
    ):
        for key, label in labels.items():
            value = hostlist.get(group, {}).get(key)
            if value is not None:
                outcomes.append((label, number(value - before.get(group, {}).get(key, 0))))
    event_table = table(("Outcome", "Count"), outcomes, "Connection data unavailable.")
    relay_table = table(
        ("Metric", "Value"),
        [
            ("Mint status", relay_status),
            ("Reason", relay.get("why", "—")),
            ("Used (GB)", number(used_gb) if used_gb is None or used_gb >= 0.01 else f"{used_gb:.6f}"),
            ("Threshold (GB)", number(budget_gb)),
            ("Remaining (GB)", number(remaining)),
            ("Usage read (UTC)", relay.get("readAt", "—")),
        ],
        "",
    )
    probe_section = ""
    if probes := hostlist.get("probes"):
        prior = before.get("probes", {})
        results = [
            (key, value - prior.get("results", {}).get(key, 0)) for key, value in sorted(probes["results"].items())
        ]
        first_frames = sum(count for key, count in results if key.endswith("/first_frame"))
        first_ms = probes["firstFrameMs"] - prior.get("firstFrameMs", 0)
        received = probes["received"] - prior.get("received", 0)
        issued = probes["issued"] - prior.get("issued", 0)
        average = number(first_ms / first_frames) if first_frames else "—"
        result_table = table(("Game / path / stage", "Probes"), results, "No probe outcomes in this sample.")
        builds = table(
            ("Build", "Reports"),
            [(key, value - prior.get("builds", {}).get(key, 0)) for key, value in sorted(probes["builds"].items())],
            "No reported builds.",
        )
        probe_section = (
            f'<section><h2>Opt-in peer probes</h2><p class="note">{window} · {received} reports, {issued} receipts '
            f"issued · Mean time to first frame {average} ms. A probe can run before the player selects a match.</p>"
            f"{result_table}{builds}</section>"
        )
    start, end = escape(str(snapshot.get("from", "—"))), escape(str(snapshot.get("to", "—")))
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>FCI multiplayer monitoring</title>
<style>
:root {{color-scheme:light;--ink:#18212d;--muted:#506172;--line:#d6dee6;--surface:#fff;--accent:#135d64}}
* {{box-sizing:border-box}}
body {{margin:0;background:#eef3f5;color:var(--ink);font:15px/1.5 "Instrument Sans",system-ui,sans-serif}}
header {{background:#142d33;color:#fff;padding:32px max(24px,calc((100vw - 1120px)/2))}}
h1,h2,p {{margin:0}} h1 {{font-size:clamp(27px,5vw,40px);line-height:1.15;letter-spacing:-.025em}} h2 {{font-size:19px}}
header p {{color:#d1e5e5;margin-top:8px}} main {{max-width:1168px;margin:auto;padding:24px}}
.cards,.columns {{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:14px;margin-bottom:16px}}
.card,section {{background:var(--surface);border:1px solid var(--line);border-radius:9px;padding:16px;min-width:0}}
.card span,.card small,.note {{color:var(--muted)}} .card strong {{display:block;font-size:29px;color:var(--accent)}}
.card strong,td {{font-variant-numeric:tabular-nums}} section {{margin-bottom:16px}} .columns section {{margin:0}}
.note {{margin:5px 0}} .notice {{margin:0 0 16px;padding:10px 16px;background:#fff1d5;border-radius:6px}}
ul.notice {{padding-left:32px}} .scroll {{overflow:auto}}
table {{border-collapse:collapse;width:100%;margin-top:8px;text-align:left}}
th,td {{padding:8px 9px;border-bottom:1px solid var(--line);white-space:nowrap}}
th {{font-size:12px;color:var(--muted)}}
</style></head><body>
<header><h1>FCI multiplayer monitoring</h1><p>Cloudflare window · {start} to {end}</p></header>
<main>{warning}<div class="cards">{cards_html}</div>
<section><h2>Host-list routes</h2><p class="note">{window} · Lobby started {time_label(started)}
 · Long-poll latency includes waiting.</p>{routes}</section>
<div class="columns"><section><h2>Active listings</h2>
<p class="note">A listing can be a lobby or a match.</p>{listings}</section>
<section><h2>Connection handoff</h2>
<p class="note">{window}. Delivery counts host notifications.</p>{event_table}</section>
<section><h2>TURN budget</h2>
<p class="note">Month to date · {escape(str(relay.get("month", "—")))}</p>{relay_table}</section></div>
{probe_section}
<p class="note">Browsing players, players in matches, and matches in progress are currently unavailable.</p>
</main></body></html>
"""

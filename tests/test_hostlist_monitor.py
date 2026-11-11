from argparse import Namespace
from datetime import UTC, datetime
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

SPEC = importlib.util.spec_from_file_location(
    "hostlist_monitor", Path(__file__).resolve().parents[1] / "tools/hostlist/monitor.py"
)
monitor = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(monitor)


def test_snapshot_keeps_available_metrics_when_one_query_fails(monkeypatch):
    def request(url, credential, payload=None):
        if url.endswith("/admin/relay"):
            return {"usedBytes": 7, "allow": True}
        if url.endswith("/admin/metrics"):
            return {"active": {"instincts/2": 1}}
        assert url == monitor.GRAPHQL
        if "durableObjectsPeriodicGroups" in payload["query"]:
            assert payload["variables"]["name"] == "namespace"
            return {"errors": [{"message": "provider-specific detail"}]}
        assert "scriptName: $name" in payload["query"]
        assert payload["variables"]["name"] == "worker"
        return {
            "data": {"viewer": {"accounts": [{"workersInvocationsAdaptive": [{"sum": {"requests": 42, "errors": 1}}]}]}}
        }

    monkeypatch.setattr(monitor, "request_json", request)
    result = monitor.collect(
        Namespace(hours=24, script="worker", namespace="namespace", url="http://127.0.0.1:9000"),
        {"CLOUDFLARE_ACCOUNT_ID": "account", "CLOUDFLARE_API_TOKEN": "test", "FCI_HOSTLIST_ADMIN_SECRET": "test-admin"},
        datetime(2026, 11, 11, 13, tzinfo=UTC),
    )
    assert result["workers"] == {"requests": 42, "errors": 1}
    assert result["errors"] == {"durableObject": "Cloudflare rejected the analytics query"}
    assert result["hostlist"]["active"] == {"instincts/2": 1}
    assert result["relay"]["usedBytes"] == 7
    assert "test-admin" not in str(result)


def test_html_report_compares_one_lobby_and_preserves_missing_sections(tmp_path):
    snapshot = {
        "errors": {"durableObject": "<unavailable>"},
        "workers": {"requests": 15, "errors": 0},
        "hostlist": {
            "startedAt": 1000,
            "sampledAt": 3000,
            "active": {"instincts/2": 1},
            "routes": {"GET /v1/hosts": {"requests": 6, "totalMs": 31, "maxMs": 14, "status": {"200": 5, "503": 1}}},
        },
    }
    baseline = {
        "hostlist": {
            "startedAt": 1000,
            "sampledAt": 2000,
            "routes": {"GET /v1/hosts": {"requests": 2, "totalMs": 11, "maxMs": 8, "status": {"200": 2}}},
        },
    }
    source, previous, report = (tmp_path / name for name in ("sample.json", "before.json", "report.html"))
    source.write_text(json.dumps(snapshot), encoding="utf-8")
    previous.write_text(json.dumps(baseline), encoding="utf-8")
    command = [
        sys.executable,
        "-B",
        monitor.__file__,
        "--input",
        str(source),
        "--html",
        str(report),
        "--baseline",
        str(previous),
    ]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    assert result.returncode == 1  # The unavailable usage metric remains visible to callers.
    page = report.read_text(encoding="utf-8")
    assert "Since baseline" in page
    assert "<td>4</td><td>1</td><td>25.0%</td><td>5</td><td>14</td>" in page
    assert "&lt;unavailable&gt;" in page
    baseline["hostlist"]["startedAt"] = 0
    previous.write_text(json.dumps(baseline), encoding="utf-8")
    subprocess.run(command, capture_output=True, text=True, check=False)
    assert "Showing current totals." in report.read_text(encoding="utf-8")

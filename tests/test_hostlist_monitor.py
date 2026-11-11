from argparse import Namespace
from datetime import UTC, datetime
import importlib.util
from pathlib import Path

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

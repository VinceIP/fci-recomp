"""tools/msvc.py and tools/toolchain.py on synthetic manifests and folders (nothing is downloaded)"""

import json
from pathlib import Path
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import msvc  # noqa: E402
import toolchain  # noqa: E402


def payload(name):
    return {"fileName": name, "url": "https://example.invalid/" + name, "sha256": "0" * 64, "size": 1}


def packages(sdk_files, sep="\\"):
    out = {f"microsoft.vc.{msvc.MSVC}.{n}": {"payloads": [payload(f"{n}.vsix")]} for n in msvc.MSVC_PACKAGES}
    out[f"win11sdk_{msvc.SDK}"] = {"payloads": [payload("Installers" + sep + f) for f in sdk_files]}
    return out


def test_plan_picks_the_pinned_packages():
    cab = "0123456789abcdef0123456789abcdef.cab"
    for sep in ("\\", "/"):  # the manifest has used both
        vsix, msis, by_name = msvc.plan(packages(msvc.SDK_INSTALLERS + [cab, "Orca-x86_en-us.msi"], sep))
        assert [p["fileName"] for p in vsix] == [f"{n}.vsix" for n in msvc.MSVC_PACKAGES]
        assert [msvc.file_name(p) for p in msis] == msvc.SDK_INSTALLERS
        assert cab in by_name


def test_plan_names_what_is_missing():
    try:
        msvc.plan(packages(msvc.SDK_INSTALLERS[1:]))
    except msvc.DownloadError as e:
        assert msvc.SDK_INSTALLERS[0] in str(e)
        return
    raise AssertionError("expected DownloadError")


def test_cabs_of_reads_the_media_names(tmp_path):
    msi = tmp_path / "a.msi"
    msi.write_bytes(b"\0junk 0123456789abcdef0123456789abcdef.cab\0fedcba9876543210fedcba9876543210.cab x.cab")
    assert msvc.cabs_of(msi) == ["0123456789abcdef0123456789abcdef.cab", "fedcba9876543210fedcba9876543210.cab"]


def test_unpack_vsix_lays_out_contents(tmp_path):
    vsix = tmp_path / "p.vsix"
    with zipfile.ZipFile(vsix, "w") as z:
        z.writestr("Contents/VC/Tools/MSVC/14.51.1/bin/Hostx64/x64/cl.exe", b"cl")
        z.writestr("manifest.json", b"{}")
    msvc.unpack_vsix(vsix, tmp_path / "root")
    assert (tmp_path / "root/VC/Tools/MSVC/14.51.1/bin/Hostx64/x64/cl.exe").read_bytes() == b"cl"
    assert not (tmp_path / "root/manifest.json").exists()


def test_toolchain_reads_a_download(tmp_path):
    assert toolchain.downloaded(tmp_path) is None
    (tmp_path / "msvc.json").write_text(
        json.dumps(
            {
                "msvc": msvc.MSVC + ".1",
                "windows_sdk": "10.0.26100",
                "sdk_installers": msvc.SDK_INSTALLERS,
                "msvc_packages": msvc.MSVC_PACKAGES,
            }
        )
    )
    (tmp_path / "Windows Kits/10/Include/10.0.26100.0").mkdir(parents=True)
    tc = toolchain.downloaded(tmp_path)
    assert tc["msvc"] == msvc.MSVC + ".1" and tc["windows_sdk"] == "10.0.26100.0"
    assert tc["cl"].endswith(str(Path(f"VC/Tools/MSVC/{msvc.MSVC}.1/bin/Hostx64/x64/cl.exe")))


def test_msiexec_quotes_each_path_its_own_way(tmp_path):
    root = tmp_path / "Far Cry Instincts" / "work"
    cmd = msvc.msiexec_command(root / "a b.msi", root, root / "msiexec.log")
    assert cmd == f'msiexec.exe /a "{root / "a b.msi"}" /qn /l*v "{root / "msiexec.log"}" TARGETDIR="{root}"'
    assert '"TARGETDIR=' not in cmd  # the form that opens msiexec's usage window


def test_the_pin_is_for_these_packages():
    pin = msvc.pinned()
    assert pin, "data/msvc.json pins other packages than tools/msvc.py selects: run tools/msvc.py --pin"
    assert [p["fileName"] for p in pin["sdk_installers"]] == msvc.SDK_INSTALLERS
    for p in pin["msvc_payloads"] + pin["sdk_installers"] + pin["sdk_cabs"]:
        assert p["url"].startswith("https://download.visualstudio.microsoft.com/")
        assert len(p["sha256"]) == 64 and p["size"] > 0
    assert all(len(p["fileName"]) == 36 and p["fileName"].endswith(".cab") for p in pin["sdk_cabs"])


def fake_unpack(calls, fail=None):
    def unpack(root, vsix, msis, cabs, log, progress):
        calls.append((vsix, msis, cabs))
        if fail and len(calls) == 1:
            raise fail
        (root / f"VC/Tools/MSVC/{msvc.MSVC}.1/bin/Hostx64/x64").mkdir(parents=True, exist_ok=True)
        (root / f"VC/Tools/MSVC/{msvc.MSVC}.1/bin/Hostx64/x64/cl.exe").write_bytes(b"")

    return unpack


def test_fetch_takes_the_pin_without_reading_the_catalog(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(msvc, "manifest", lambda: (_ for _ in ()).throw(AssertionError("catalog read")))
    monkeypatch.setattr(msvc, "unpack", fake_unpack(calls))
    info = msvc.fetch(tmp_path, accept_license=True, log=lambda *a: None)
    pin = msvc.pinned()
    assert calls == [(pin["msvc_payloads"], pin["sdk_installers"], {p["fileName"]: p for p in pin["sdk_cabs"]})]
    assert info["license"] == pin["license"] and info["msvc"] == msvc.MSVC + ".1"


def test_fetch_falls_back_to_the_catalog_when_a_pinned_file_is_gone(tmp_path, monkeypatch):
    import urllib.error

    calls = []
    gone = urllib.error.HTTPError("https://download.visualstudio.microsoft.com/x", 404, "Not Found", {}, None)
    catalog = packages(msvc.SDK_INSTALLERS, "/")
    monkeypatch.setattr(msvc, "manifest", lambda: (catalog, "https://example.invalid/license"))
    monkeypatch.setattr(msvc, "unpack", fake_unpack(calls, fail=gone))
    msvc.fetch(tmp_path, accept_license=True, log=lambda *a: None)
    assert len(calls) == 2 and calls[1] == msvc.plan(catalog)


def test_the_downloaded_toolchain_builds_a_program(tmp_path):
    """the weekly compiler workflow: the headers and libraries the game's runtime uses, from a real download"""
    import os
    import subprocess

    import pytest

    folder = os.environ.get("FCI_DOWNLOADED_TOOLCHAIN")
    if not folder:
        pytest.skip("set FCI_DOWNLOADED_TOOLCHAIN to a tools/msvc.py download")
    tc = toolchain.downloaded(Path(folder))
    assert tc, f"no finished download in {folder}"
    (tmp_path / "t.cpp").write_text(
        "#include <windows.h>\n#include <windowsx.h>\n#include <d3d12.h>\n#include <dxgi1_6.h>\n"
        "#include <xaudio2.h>\n#include <wrl/client.h>\n#include <stdio.h>\n"
        "int main() { Microsoft::WRL::ComPtr<IDXGIFactory4> f;"
        ' printf("%d\\n", SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))); return 0; }\n'
    )
    env = dict(os.environ, INCLUDE=";".join(tc["include"] + [tc["winrt"]]), LIB=";".join(tc["lib"]))
    argv = [tc["cl"], "/nologo", "/EHsc", "/MT", "/O2", "t.cpp", "d3d12.lib", "dxgi.lib", "xaudio2.lib", "ole32.lib"]
    built = subprocess.run(argv, cwd=tmp_path, env=env, capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    assert subprocess.run([str(tmp_path / "t.exe")], capture_output=True).returncode == 0

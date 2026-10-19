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


def packages(sdk_files):
    out = {f"microsoft.vc.{msvc.MSVC}.{n}": {"payloads": [payload(f"{n}.vsix")]} for n in msvc.MSVC_PACKAGES}
    out[f"win11sdk_{msvc.SDK}"] = {"payloads": [payload("Installers\\" + f) for f in sdk_files]}
    return out


def test_plan_picks_the_pinned_packages():
    cab = "0123456789abcdef0123456789abcdef.cab"
    vsix, msis, by_name = msvc.plan(packages(msvc.SDK_INSTALLERS + [cab, "Orca-x86_en-us.msi"]))
    assert [p["fileName"] for p in vsix] == [f"{n}.vsix" for n in msvc.MSVC_PACKAGES]
    assert [p["fileName"].split("\\")[-1] for p in msis] == msvc.SDK_INSTALLERS
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

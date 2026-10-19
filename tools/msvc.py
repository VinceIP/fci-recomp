"""Download Microsoft's C/C++ compiler and the Windows SDK into a folder, without an installer.

  python -B tools/msvc.py <folder> [--accept-license]

For players without Visual Studio. The packages come from Microsoft's own Visual Studio package
manifest (the one the Visual Studio Installer reads) and are checked against its SHA-256 sums:
MSVC's x64 compiler, linker and static libraries, and the Windows SDK's headers and libraries.
Nothing is installed or registered; administrator rights are not needed (the SDK's .msi files are
unpacked with an administrative install, msiexec /a, into the folder). About 400 MB to download,
1.5 GB unpacked. Microsoft's license terms for the Build Tools apply; they are shown first.

The versions are pinned to the ones this project's builds were checked with.
"""

import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

CHANNEL = "https://aka.ms/vs/18/stable/channel"
MSVC = "14.51"
SDK = "10.0.26100"
# the static CRT is in desktop.base; oldnames.lib, chkstk.obj and the other common objects in store.base
MSVC_PACKAGES = [
    "tools.hostx64.targetx64.base",
    "tools.hostx64.targetx64.res.base",
    "crt.headers.base",
    "crt.x64.desktop.base",
    "crt.x64.store.base",
]
# the shared headers are split between the Store Apps OnecoreUap package (winapifamily.h, sdkddkver.h)
# and the x86 OnecoreUap headers (windowsx.h, ntstatus.h); d3d12.h, xaudio2.h and wrl are in the Store
# Apps headers; the import libraries in the Desktop and Store Apps libraries
SDK_INSTALLERS = [
    "Windows SDK for Windows Store Apps Headers-x86_en-us.msi",
    "Windows SDK for Windows Store Apps Headers OnecoreUap-x86_en-us.msi",
    "Windows SDK for Windows Store Apps Libs-x86_en-us.msi",
    "Windows SDK Desktop Headers x86-x86_en-us.msi",
    "Windows SDK OnecoreUap Headers x86-x86_en-us.msi",
    "Windows SDK Desktop Headers x64-x86_en-us.msi",
    "Windows SDK Desktop Libs x64-x86_en-us.msi",
    "Universal CRT Headers Libraries and Sources-x86_en-us.msi",
]
DONE = "msvc.json"


class DownloadError(Exception):
    pass


def fetch_json(url):
    with urllib.request.urlopen(url) as r:
        return json.loads(r.read().decode("utf-8-sig"))


def download(payload, folder):
    """one manifest payload into folder, checked against its SHA-256; kept if already complete"""
    target = folder / payload["fileName"].replace("\\", "/").split("/")[-1]
    if target.exists() and target.stat().st_size == payload["size"]:
        return target
    digest = hashlib.sha256()
    partial = target.with_name(target.name + ".partial")
    with urllib.request.urlopen(payload["url"]) as r, open(partial, "wb") as f:
        while chunk := r.read(1 << 20):
            digest.update(chunk)
            f.write(chunk)
    if digest.hexdigest().lower() != payload["sha256"].lower():
        partial.unlink()
        raise DownloadError(f"{payload['fileName']}: the download does not match Microsoft's checksum")
    partial.replace(target)
    return target


def manifest():
    channel = fetch_json(CHANNEL)
    items = {i["id"]: i for i in channel["channelItems"]}
    license_url = next(
        (
            r["license"]
            for r in items["Microsoft.VisualStudio.Product.BuildTools"].get("localizedResources", [])
            if r["language"] == "en-us"
        ),
        None,
    )
    vs = fetch_json(items["Microsoft.VisualStudio.Manifests.VisualStudio"]["payloads"][0]["url"])
    packages = {}
    for p in vs["packages"]:
        if p.get("language") in (None, "neutral", "en-US"):
            packages.setdefault(p["id"].lower(), p)
    return packages, license_url


def plan(packages):
    """the payloads to fetch: (MSVC .vsix list, SDK .msi list, SDK .cab payloads by name)"""
    vsix = []
    for name in MSVC_PACKAGES:
        p = packages.get(f"microsoft.vc.{MSVC}.{name}")
        if not p:
            raise DownloadError(
                f"MSVC {MSVC} ({name}) is no longer offered by Microsoft's manifest; "
                "install Visual Studio Build Tools instead"
            )
        vsix += p["payloads"]
    sdk = packages.get(f"win11sdk_{SDK}")
    if not sdk:
        raise DownloadError(f"Windows SDK {SDK} is no longer offered; install Visual Studio Build Tools instead")
    by_name = {pl["fileName"].split("\\")[-1]: pl for pl in sdk["payloads"]}
    missing = [m for m in SDK_INSTALLERS if m not in by_name]
    if missing:
        raise DownloadError(f"the Windows SDK package lacks {missing}")
    return vsix, [by_name[m] for m in SDK_INSTALLERS], by_name


def unpack_vsix(path, root):
    """a .vsix is a zip; its Contents/ folder is laid out from the Visual Studio root"""
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            if not info.filename.startswith("Contents/") or info.is_dir():
                continue
            out = root / info.filename[len("Contents/") :].replace("%20", " ")
            if out.name.lower() == "vctip.exe":
                continue  # the compiler's usage telemetry: it would start, and outlive every build
            if out.exists() and out.stat().st_size == info.file_size:
                continue  # unpacked by an earlier run
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(out, "wb") as dst:
                shutil.copyfileobj(src, dst)


def cabs_of(msi):
    """the .cab files an SDK .msi names (its media table): 32 hex digits and .cab"""
    return sorted(set(m.decode() for m in re.findall(rb"[0-9a-f]{32}\.cab", msi.read_bytes())))


def msiexec_command(msi, target, log):
    """one command line: msiexec wants PROPERTY="value", and opens its usage window (even with /qn) for
    the "PROPERTY=value" a list argument with a space in the path is quoted as"""
    return f'msiexec.exe /a "{msi}" /qn /l*v "{log}" TARGETDIR="{target}"'


def current(info):
    """a finished download of the packages this version of the script selects"""
    return (
        info.get("sdk_installers") == SDK_INSTALLERS
        and info.get("msvc_packages") == MSVC_PACKAGES
        and info.get("msvc", "").startswith(MSVC + ".")
    )


def fetch(root, accept_license=False, log=print):
    root = Path(root).resolve()
    record = root / DONE
    accepted_before = record.exists()  # the record keeps the license the user accepted
    if accepted_before:
        info = json.loads(record.read_text(encoding="utf-8"))
        if current(info):
            return info
        # an older selection of packages: fetch again over it (the record is replaced at the end)
    packages, license_url = manifest()
    if not accept_license and not accepted_before:
        print(
            "Microsoft's C++ compiler and the Windows SDK are needed to build the game. They are "
            "downloaded from Microsoft\nunder the Visual Studio Build Tools license terms:\n  "
            f"{license_url}\n"
        )
        if input("Do you accept these license terms? (y/n) ").strip().lower() not in ("y", "yes"):
            raise DownloadError("the license terms were not accepted; nothing was downloaded")
    vsix, msis, sdk_payloads = plan(packages)
    root.mkdir(parents=True, exist_ok=True)
    cache = root / "downloads"
    cache.mkdir(exist_ok=True)
    log(f"  MSVC {MSVC}: {sum(p['size'] for p in vsix) // 1_000_000} MB")
    for p in vsix:
        unpack_vsix(download(p, cache), root)
    log(f"  Windows SDK {SDK}")
    with tempfile.TemporaryDirectory(dir=root) as tmp:
        sdk_dir = Path(tmp) / "Installers"
        sdk_dir.mkdir()
        for p in msis:
            msi = download(p, sdk_dir)
            for cab in cabs_of(msi):
                if cab not in sdk_payloads:
                    raise DownloadError(f"{msi.name} needs {cab}, which the SDK package lacks")
                download(sdk_payloads[cab], sdk_dir)
        for p in msis:
            msi = sdk_dir / p["fileName"].split("\\")[-1]
            log = root / "msiexec.log"
            done = subprocess.run(msiexec_command(msi, root, log))
            if done.returncode:
                raise DownloadError(f"unpacking {msi.name} failed (msiexec {done.returncode}); its log is {log}")
            log.unlink(missing_ok=True)
            (root / msi.name).unlink(missing_ok=True)  # the administrative install copies the .msi too
    shutil.rmtree(cache)
    tools = sorted((root / "VC" / "Tools" / "MSVC").glob(MSVC + ".*"))
    if not tools or not (tools[-1] / "bin" / "Hostx64" / "x64" / "cl.exe").exists():
        raise DownloadError("the compiler was not found after unpacking")
    info = {
        "msvc": tools[-1].name,
        "windows_sdk": SDK,
        "msvc_packages": MSVC_PACKAGES,
        "sdk_installers": SDK_INSTALLERS,
        "license": license_url,
    }
    record.write_text(json.dumps(info, indent=1) + "\n", encoding="utf-8")
    return info


def main(argv):
    if not argv or argv[0].startswith("-"):
        raise SystemExit(__doc__)
    try:
        info = fetch(argv[0], accept_license="--accept-license" in argv)
    except DownloadError as e:
        raise SystemExit(f"error: {e}")
    print(f"MSVC {info['msvc']} and Windows SDK {info['windows_sdk']} in {argv[0]}")


if __name__ == "__main__":
    main(sys.argv[1:])

"""Download Microsoft's C/C++ compiler and the Windows SDK into a folder, without an installer.

  python -B tools/msvc.py <folder> [--accept-license]
  python -B tools/msvc.py --pin      record the current downloads in data/msvc.json (at a release)
  python -B tools/msvc.py --check    every pinned download still there, the catalog still usable

For players without Visual Studio: MSVC's x64 compiler, linker and static libraries, and the Windows
SDK's headers and libraries, from Microsoft's own Visual Studio package manifest (the one the Visual
Studio Installer reads). data/msvc.json pins each download's address, size and SHA-256, so a change
to Microsoft's catalog cannot break a release; only when a pinned file is gone is the live catalog
read instead.
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
import time
import urllib.error
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
PINNED = Path(__file__).resolve().parents[1] / "data" / "msvc.json"


class DownloadError(Exception):
    pass


def fetch_json(url):
    with urllib.request.urlopen(url) as r:
        return json.loads(r.read().decode("utf-8-sig"))


def file_name(payload):
    """a payload's file name; the manifest separates folders with either slash (SDK 26100.18 switched to /)"""
    return re.split(r"[\\/]", payload["fileName"])[-1]


def download(payload, folder):
    """one manifest payload into folder, checked against its SHA-256; kept if already complete"""
    target = folder / file_name(payload)
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
    by_name = {file_name(pl): pl for pl in sdk["payloads"]}
    missing = [m for m in SDK_INSTALLERS if m not in by_name]
    if missing:
        raise DownloadError(f"the Windows SDK package lacks {missing}")
    return vsix, [by_name[m] for m in SDK_INSTALLERS], by_name


def served_size(url):
    """the size Microsoft serves: the catalog's "size" of a .vsix is not its download's"""
    with urllib.request.urlopen(urllib.request.Request(url, method="HEAD")) as r:
        return int(r.headers.get("Content-Length", -1))


def entry(payload):
    """what a pin keeps of a manifest payload"""
    return {
        "fileName": file_name(payload),
        "url": payload["url"],
        "sha256": payload["sha256"].lower(),
        "size": served_size(payload["url"]),
    }


def pinned(path=PINNED):
    """data/msvc.json, when it pins the packages this version of the script selects"""
    try:
        pin_data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    same = (
        pin_data.get("msvc") == MSVC
        and pin_data.get("windows_sdk") == SDK
        and pin_data.get("msvc_packages") == MSVC_PACKAGES
        and [p["fileName"] for p in pin_data.get("sdk_installers", [])] == SDK_INSTALLERS
    )
    return pin_data if same else None


def pin(path=PINNED, log=print):
    """the catalog's current downloads, with the .cab files each SDK installer names, into data/msvc.json"""
    packages, license_url = manifest()
    vsix, msis, by_name = plan(packages)
    cabs = set()
    with tempfile.TemporaryDirectory() as tmp:
        for p in msis:
            cabs.update(cabs_of(download(p, Path(tmp))))
    missing = sorted(cabs - by_name.keys())
    if missing:
        raise DownloadError(f"the SDK installers need {missing}, which the SDK package lacks")
    data = {
        "comment": "Microsoft's compiler and Windows SDK as tools/msvc.py downloads them for builds without Visual "
        "Studio: each file's address, size and SHA-256, from the Visual Studio catalog when tools/msvc.py --pin "
        "last ran. Pinned again at each release.",
        "msvc": MSVC,
        "msvc_version": packages[f"microsoft.vc.{MSVC}.{MSVC_PACKAGES[0]}"].get("version"),
        "windows_sdk": SDK,
        "windows_sdk_version": packages[f"win11sdk_{SDK}"].get("version"),
        "license": license_url,
        "msvc_packages": MSVC_PACKAGES,
        "msvc_payloads": [entry(p) for p in vsix],
        "sdk_installers": [entry(p) for p in msis],
        "sdk_cabs": [entry(by_name[c]) for c in sorted(cabs)],
    }
    Path(path).write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8", newline="\n")
    log(f"MSVC {data['msvc_version']}, Windows SDK {data['windows_sdk_version']}, {len(cabs)} .cab files: {path}")
    return data


def check(path=PINNED, log=print):
    """what is wrong with the pin: a pinned file gone or changed in size, or the catalog no longer usable as
    the fallback. CI runs it every week (.github/workflows/compiler.yml)"""
    pin_data = pinned(path)
    if not pin_data:
        return [f"{path} does not pin the packages tools/msvc.py selects; run tools/msvc.py --pin"]
    problems = []
    for p in pin_data["msvc_payloads"] + pin_data["sdk_installers"] + pin_data["sdk_cabs"]:
        for attempt in range(3):  # a dropped connection is not a missing file
            try:
                size = served_size(p["url"])
                if size != p["size"]:
                    problems.append(f"{p['fileName']}: {size} bytes at Microsoft, {p['size']} pinned")
                break
            except urllib.error.HTTPError as e:
                problems.append(f"{p['fileName']}: {e}")
                break
            except OSError as e:
                if attempt == 2:
                    problems.append(f"{p['fileName']}: {e}")
                time.sleep(5)
    try:
        packages, _ = manifest()
        plan(packages)
        now = (
            packages[f"win11sdk_{SDK}"].get("version"),
            packages[f"microsoft.vc.{MSVC}.{MSVC_PACKAGES[0]}"].get("version"),
        )
        if now != (pin_data["windows_sdk_version"], pin_data["msvc_version"]):
            log(f"note: the catalog now has Windows SDK {now[0]} and MSVC {now[1]}; the pin still works")
    except (DownloadError, OSError, KeyError, ValueError) as e:
        problems.append(f"the catalog (the fallback): {e}")
    return problems


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


def ask_in_console(license_url):
    print(
        "Microsoft's C++ compiler and the Windows SDK are needed to build the game. They are "
        "downloaded from Microsoft\nunder the Visual Studio Build Tools license terms:\n  "
        f"{license_url}\n"
    )
    return input("Do you accept these license terms? (y/n) ").strip().lower() in ("y", "yes")


def fetch(root, accept_license=False, log=print, ask=ask_in_console, progress=None):
    """ask: called with the license terms' URL when they were not accepted before; True to go on.
    progress: called with (done, in all) as the packages arrive: the compiler's by size, then the SDK's"""
    progress = progress or (lambda done, total: None)
    root = Path(root).resolve()
    record = root / DONE
    accepted_before = record.exists()  # the record keeps the license the user accepted
    if accepted_before:
        info = json.loads(record.read_text(encoding="utf-8"))
        if current(info):
            return info
        # an older selection of packages: fetch again over it (the record is replaced at the end)
    pin_data = pinned()
    packages, license_url = (None, pin_data["license"]) if pin_data else manifest()
    if not accept_license and not accepted_before and not ask(license_url):
        raise DownloadError("the license terms were not accepted; nothing was downloaded")
    root.mkdir(parents=True, exist_ok=True)
    if pin_data:
        try:
            cabs = {p["fileName"]: p for p in pin_data["sdk_cabs"]}
            unpack(root, pin_data["msvc_payloads"], pin_data["sdk_installers"], cabs, log, progress)
        except urllib.error.HTTPError as e:
            if e.code not in (403, 404, 410):
                raise
            # a pinned file Microsoft no longer serves: its current catalog, as before the pin
            log(f"  a pinned download is gone ({e.code}, {e.url}); using Microsoft's current catalog")
            pin_data = None
    if not pin_data:
        unpack(root, *plan(packages or manifest()[0]), log, progress)
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


def unpack(root, vsix, msis, cabs, log, progress):
    """the compiler's .vsix files, and the SDK's installers with the .cab files they name, unpacked into root"""
    cache = root / "downloads"
    cache.mkdir(exist_ok=True)
    log(f"  MSVC {MSVC}: {sum(p['size'] for p in vsix) // 1_000_000} MB")
    total, done = sum(p["size"] for p in vsix), 0
    for p in vsix:
        unpack_vsix(download(p, cache), root)
        done += p["size"]
        progress(done, 2 * total)
    log(f"  Windows SDK {SDK}")
    with tempfile.TemporaryDirectory(dir=root) as tmp:
        sdk_dir = Path(tmp) / "Installers"
        sdk_dir.mkdir()
        for i, p in enumerate(msis):
            msi = download(p, sdk_dir)
            for cab in cabs_of(msi):
                if cab not in cabs:
                    raise DownloadError(f"{msi.name} needs {cab}, which the SDK package lacks")
                download(cabs[cab], sdk_dir)
            progress(total + total * (i + 1) // len(msis), 2 * total)
        for p in msis:
            msi = sdk_dir / file_name(p)
            msi_log = root / "msiexec.log"
            unpacked = subprocess.run(msiexec_command(msi, root, msi_log))
            if unpacked.returncode:
                raise DownloadError(
                    f"unpacking {msi.name} failed (msiexec {unpacked.returncode}); its log is {msi_log}"
                )
            msi_log.unlink(missing_ok=True)
            (root / msi.name).unlink(missing_ok=True)  # the administrative install copies the .msi too
    shutil.rmtree(cache)


def main(argv):
    if argv[:1] == ["--pin"]:
        pin()
        return
    if argv[:1] == ["--check"]:
        problems = check()
        for problem in problems:
            print(f"error: {problem}")
        raise SystemExit(1 if problems else 0)
    if not argv or argv[0].startswith("-"):
        raise SystemExit(__doc__)
    try:
        info = fetch(argv[0], accept_license="--accept-license" in argv)
    except DownloadError as e:
        raise SystemExit(f"error: {e}")
    print(f"MSVC {info['msvc']} and Windows SDK {info['windows_sdk']} in {argv[0]}")


if __name__ == "__main__":
    main(sys.argv[1:])

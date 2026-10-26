"""Find the C/C++ toolchain on this machine: Visual Studio's MSVC (through vswhere) and the Windows SDK,
or the copy tools/msvc.py downloaded.

  python -B tools/toolchain.py            print what it finds
  python -B tools/toolchain.py <file>     also write it as JSON (the build reads that file)

The build needs MSVC x64 (cl, link, dumpbin, the static CRT and C++ library) and a Windows 10/11 SDK
(headers, import libraries including Direct3D 12 and XAudio2). Both come with Visual Studio or its Build
Tools under "Desktop development with C++", or from tools/msvc.py without an installer.
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import winreg

VSWHERE = (
    Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
    / "Microsoft Visual Studio"
    / "Installer"
    / "vswhere.exe"
)


class ToolchainError(Exception):
    pass


def find_msvc():
    """the newest Visual Studio with the x64 C++ tools: (install path, MSVC tools folder)"""
    if not VSWHERE.exists():
        raise ToolchainError(
            "Visual Studio is not installed (no vswhere.exe). Install Visual Studio or its "
            'Build Tools with "Desktop development with C++".'
        )
    found = subprocess.run(
        [
            str(VSWHERE),
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-format",
            "json",
            "-utf8",
        ],
        capture_output=True,
        encoding="utf-8",
        check=True,
    )
    installs = json.loads(found.stdout or "[]")
    if not installs:
        raise ToolchainError(
            'No Visual Studio with the C++ x64 tools was found. Add "Desktop development with '
            'C++" in the Visual Studio Installer.'
        )
    root = Path(installs[0]["installationPath"])
    version = (root / "VC" / "Auxiliary" / "Build" / "Microsoft.VCToolsVersion.default.txt").read_text().strip()
    tools = root / "VC" / "Tools" / "MSVC" / version
    if not (tools / "bin" / "Hostx64" / "x64" / "cl.exe").exists():
        raise ToolchainError(f"MSVC {version} in {root} has no x64 compiler")
    return root, tools


def find_sdk():
    """the newest Windows 10/11 SDK with the desktop headers and x64 libraries: (root, version)"""
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows Kits\Installed Roots") as key:
            root = Path(winreg.QueryValueEx(key, "KitsRoot10")[0])
    except OSError:
        root = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10"
    versions = sorted(
        (
            p.name
            for p in (root / "Include").glob("10.*")
            if (p / "um" / "windows.h").exists() and (root / "Lib" / p.name / "um" / "x64" / "d3d12.lib").exists()
        ),
        key=lambda v: tuple(int(x) for x in v.split(".")),
    )
    if not versions:
        raise ToolchainError(
            "No Windows SDK with desktop x64 libraries was found. Add a Windows 11 SDK in the Visual Studio Installer."
        )
    return root, versions[-1]


def discover(fallback=None):
    """the installed Visual Studio and SDK; else the download in fallback (tools/msvc.py's folder)"""
    try:
        vs, msvc = find_msvc()
        sdk, version = find_sdk()
    except ToolchainError:
        found = downloaded(fallback) if fallback else None
        if found:
            return found
        raise
    return describe(vs, msvc, sdk, version)


def downloaded(folder):
    """tools/msvc.py's folder, or None when it holds no finished download"""
    folder = Path(folder)
    try:
        info = json.loads((folder / "msvc.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    import msvc

    if not msvc.current(info):
        return None
    sdk = folder / "Windows Kits" / "10"
    version = next((p.name for p in (sdk / "Include").glob(info["windows_sdk"] + ".*")), None)
    if not version:
        return None
    return describe(folder, folder / "VC" / "Tools" / "MSVC" / info["msvc"], sdk, version)


def describe(vs, msvc, sdk, version):
    bin_dir = msvc / "bin" / "Hostx64" / "x64"
    include = sdk / "Include" / version
    lib = sdk / "Lib" / version
    return {
        "visual_studio": str(vs),
        "msvc": msvc.name,
        "windows_sdk": version,
        "cl": str(bin_dir / "cl.exe"),
        "link": str(bin_dir / "link.exe"),
        "dumpbin": str(bin_dir / "dumpbin.exe"),
        # C headers (the build's order: compiler, then ucrt, shared, um); winrt for wrl/client.h
        "include": [str(msvc / "include"), str(include / "ucrt"), str(include / "shared"), str(include / "um")],
        "winrt": str(include / "winrt"),
        "lib": [str(msvc / "lib" / "x64"), str(lib / "ucrt" / "x64"), str(lib / "um" / "x64")],
    }


def main(argv):
    try:
        tc = discover()
    except ToolchainError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    text = json.dumps(tc, indent=2)
    print(text)
    if argv:
        Path(argv[0]).write_text(text + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

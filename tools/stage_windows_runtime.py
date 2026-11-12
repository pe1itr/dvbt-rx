#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Rob Hardenberg / PE1ITR
"""Stage a Fedora/MinGW Windows release and record DLL/source-package metadata."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

SYSTEM_DLLS = set("""advapi32 authz avrt bcrypt cabinet cfgmgr32 comctl32 comdlg32
crypt32 d2d1 d3d9 d3d11 d3d12 dbghelp dcomp dnsapi dsound dwmapi dwrite dxgi
gdi32 glu32 hid imm32 iphlpapi kernel32 mf mfplat mfreadwrite mpr msvcrt
mswsock ncrypt netapi32 normaliz ntdll ole32 oleaut32 opengl32 powrprof
propsys psapi rpcrt4 secur32 setupapi shcore shell32 shlwapi synchronization ucrtbase
user32 userenv usp10 uxtheme version winhttp wininet winmm winspool wintrust
ws2_32 wtsapi32""".split())


def imports(path):
    output = subprocess.check_output(["x86_64-w64-mingw32-objdump", "-p", str(path)], text=True)
    return re.findall(r"DLL Name:\s*(\S+)", output)


def package_info(path):
    query = subprocess.run(["rpm", "-qf", "--qf", "%{NAME}|%{SOURCERPM}|%{LICENSE}", str(path)],
                           text=True, capture_output=True)
    if query.returncode:
        return None
    name, source, license_name = query.stdout.split("|", 2)
    source_name, version, release = source.removesuffix(".src.rpm").rsplit("-", 2)
    url = f"https://kojipkgs.fedoraproject.org/packages/{source_name}/{version}/{release}/src/{source}"
    return dict(name=name, source_rpm=source, source_url=url, license=license_name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build-win"))
    parser.add_argument("--rtlsdr-dir", type=Path, required=True)
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument("--sysroot", type=Path, default=Path("/usr/x86_64-w64-mingw32/sys-root/mingw"))
    args = parser.parse_args()
    package = args.package_dir.resolve()
    package.mkdir(parents=True, exist_ok=True)
    roots = [args.rtlsdr_dir.resolve(), args.sysroot / "bin", args.build_dir.resolve()]
    available = {}
    for directory in roots:
        for path in directory.glob("*.dll"):
            available.setdefault(path.name.lower(), path)
    pending = [(args.build_dir / name, Path(name)) for name in ("rbdvbt_gui.exe", "rbdvbt_rx.exe")]
    pending += [(args.rtlsdr_dir / "rtl_sdr.exe", Path("rtl_sdr.exe"))]
    pending += [(args.sysroot / "lib/qt6/plugins/platforms/qwindows.dll", Path("platforms/qwindows.dll"))]
    inventory = {}
    while pending:
        source, destination = pending.pop()
        key = str(destination).lower()
        if key in inventory:
            continue
        if not source.is_file():
            raise SystemExit(f"Missing release input: {source}")
        target = package / destination
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        metadata = package_info(source)
        inventory[key] = dict(file=str(destination), sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
                              package=metadata)
        if metadata:
            # Preserve the notices actually installed by the binary package.
            files = subprocess.check_output(["rpm", "-ql", metadata["name"]], text=True).splitlines()
            for name in files:
                notice = Path(name)
                legal = name.startswith("/usr/share/licenses/")
                readme = name.startswith("/usr/share/doc/") and notice.name.lower() in (
                    "readme", "readme.txt", "readme.md", "authors", "copyright", "copying", "license")
                if (legal or readme) and notice.is_file():
                    relative = (notice.relative_to("/usr/share/licenses") if legal else
                                Path("doc") / notice.relative_to("/usr/share/doc"))
                    copy = package / "licenses/third-party" / relative
                    copy.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(notice, copy)
        for name in imports(target):
            lower = name.lower()
            if lower.startswith(("api-ms-win-", "ext-ms-win-")) or lower.removesuffix(".dll") in SYSTEM_DLLS:
                continue
            if lower not in available:
                raise SystemExit(f"Unresolved DLL {name}, imported by {destination}")
            pending.append((available[lower], Path(available[lower].name)))
    result = dict(files=sorted(inventory.values(), key=lambda entry: entry["file"]))
    (package / "RUNTIME-SOURCES.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"Staged {len(inventory)} Windows executables/plugins/DLLs in {package}")


if __name__ == "__main__":
    main()

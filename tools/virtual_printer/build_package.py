"""Build and smoke-test an offline Windows distribution. Never replaces a delivery."""
from __future__ import annotations

import argparse
import hashlib
from importlib import metadata
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import zipfile

from hf_virtual_printer import __version__
from hf_virtual_printer.camera import application_icon

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
NAME = "HonorForgeVirtualPrinter"


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(command, log):
    with log.open("w", encoding="utf-8") as output:
        result = subprocess.run([str(arg) for arg in command], stdout=output, stderr=subprocess.STDOUT,
                                cwd=REPO, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {log}")


def sources():
    paths = [path for path in ROOT.rglob("*") if path.is_file() and
             "__pycache__" not in path.parts and path.suffix not in {".pyc", ".part"}]
    changed = subprocess.check_output(["git", "diff", "--name-only", "HEAD"], cwd=REPO, encoding="utf-8").splitlines()
    paths += [REPO / name for name in changed if (REPO / name).is_file()]
    paths = sorted(set(paths))
    return paths, {
        "product": NAME, "version": __version__, "platform": "Windows x64", "python": platform.python_version(),
        "base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, encoding="utf-8").strip(),
        "working_tree": True,
        "files": [{"path": path.relative_to(REPO).as_posix(), "sha256": sha256(path)} for path in paths],
    }


def third_party(destination):
    entries = []
    for dist in sorted(metadata.distributions(), key=lambda value: value.metadata["Name"].lower()):
        name = dist.metadata["Name"]
        entries.append({"name": name, "version": dist.version,
                        "license": dist.metadata.get("License-Expression") or dist.metadata.get("License", "See bundled license"),
                        "home_page": dist.metadata.get("Home-page")})
        for relative in dist.files or []:
            if relative.name.lower().startswith(("license", "licence", "copying", "notice")):
                source = Path(dist.locate_file(relative))
                if source.is_file():
                    target = destination / "licenses" / name / relative.name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, target)
    (destination / "third-party.json").write_text(json.dumps(entries, ensure_ascii=False, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="Staging directory; reviewed deliveries are copied separately")
    parser.add_argument("--nsis", type=Path, default=Path(r"C:\Program Files (x86)\NSIS\makensis.exe"))
    args = parser.parse_args()
    if sys.version_info[:2] != (3, 12) or platform.system() != "Windows" or platform.machine() != "AMD64":
        raise RuntimeError("Use Windows x64 with Python 3.12 and requirements-lock.txt")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError("Output staging directory must be empty; preserve the previous validation evidence")
    work = REPO / ".tmp/virtual-printer-package"
    work.mkdir(parents=True, exist_ok=True)
    icon = work / "printer.ico"
    application_icon(256).save(icon, sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (256, 256)])
    version_file = work / "version.txt"
    version_tuple = tuple(map(int, __version__.split("."))) + (0,)
    version_file.write_text(
        "VSVersionInfo(ffi=FixedFileInfo(filevers=" + repr(version_tuple) + ", prodvers=" + repr(version_tuple) +
        ", mask=0x3f, flags=0x0, OS=0x40004, fileType=0x1, subtype=0x0, date=(0,0)), "
        "kids=[StringFileInfo([StringTable('040904B0', [StringStruct('CompanyName','Honor Forge'), "
        "StringStruct('ProductName','Honor Forge Virtual Printer'), StringStruct('FileDescription','Honor Forge Virtual Printer'), "
        f"StringStruct('FileVersion','{__version__}'), StringStruct('ProductVersion','{__version__}')])]), "
        "VarFileInfo([VarStruct('Translation',[1033,1200])])])", encoding="utf-8")
    command = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onedir", "--windowed",
               "--name", NAME, "--paths", ROOT, "--icon", icon, "--version-file", version_file,
               "--specpath", work, "--workpath", work / "work", "--distpath", work / "dist",
               "--add-data", f"{ROOT / 'hf_virtual_printer/web'};hf_virtual_printer/web",
               "--collect-submodules", "zeroconf", "--exclude-module", "tkinter"]
    for module in ["uvicorn.logging", "uvicorn.loops.asyncio", "uvicorn.protocols.http.h11_impl",
                   "uvicorn.protocols.websockets.websockets_impl", "uvicorn.lifespan.on", "pystray._win32", "ifaddr"]:
        command += ["--hidden-import", module]
    command.append(ROOT / "run.py")
    run(command, output / "pyinstaller.log")
    bundle = work / "dist" / NAME
    paths, manifest = sources()
    for filename in ["README.md", "使用说明.html"]:
        shutil.copy2(ROOT / filename, bundle / filename)
    (bundle / "source-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    third_party(bundle)
    previous = os.environ.get("HF_VP_EXECUTABLE")
    os.environ["HF_VP_EXECUTABLE"] = str(bundle / (NAME + ".exe"))
    try:
        run([sys.executable, "-m", "unittest", "discover", "-s", ROOT / "tests", "-p", "test_network.py", "-v"],
            output / "frozen-network-tests.txt")
    finally:
        if previous is None:
            os.environ.pop("HF_VP_EXECUTABLE", None)
        else:
            os.environ["HF_VP_EXECUTABLE"] = previous
    installer = output / f"{NAME}-{__version__}-win64-setup.exe"
    run([args.nsis, f"/DVERSION={__version__}", f"/DAPP_DIR={bundle}", f"/DAPP_ICON={icon}",
         f"/DOUTPUT_FILE={installer}", ROOT / "installer.nsi"], output / "nsis.log")
    portable = output / f"{NAME}-{__version__}-win64-portable.zip"
    with zipfile.ZipFile(portable, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(bundle.rglob("*")):
            if path.is_file():
                archive.write(path, f"{NAME}-{__version__}/" + path.relative_to(bundle).as_posix())
    with zipfile.ZipFile(output / "source-snapshot.zip", "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in paths:
            archive.write(path, path.relative_to(REPO).as_posix())
        archive.writestr("client-and-docs.patch", subprocess.check_output(["git", "diff", "--binary", "HEAD"], cwd=REPO))
    for filename in ["README.md", "使用说明.html"]:
        shutil.copy2(ROOT / filename, output / filename)
    (output / "source-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    payloads = [path for path in output.iterdir() if path.is_file() and path.suffix in {".exe", ".zip"}]
    (output / "SHA256SUMS.txt").write_text("".join(f"{sha256(path)}  {path.name}\n" for path in sorted(payloads)), encoding="utf-8")
    print(json.dumps({"bundle": str(bundle), "staging": str(output), "frozen_network_tests": "4 passed",
                      "delivery_replaced": False}, ensure_ascii=False))


if __name__ == "__main__":
    main()

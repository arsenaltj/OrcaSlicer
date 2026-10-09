"""Build-only offline staging from verified local dependencies and pinned wheels."""
from __future__ import annotations

import argparse
import importlib.metadata
import json
from pathlib import Path
import shutil
import sys
import zipfile

from bundled_portrait_runtime import digest, verify

WEIGHTS = {
    "mobilenet0.25_Final.pth": "2979b33ffafda5d74b6948cd7a5b9a7a62f62b949cef24e95fd15d2883a65220",
    "face_parsing.farl.celebm.main_ema_181500_jit.pt": "bbc1f0e9f68c80eb83a0b23f33850d1e10f2ec1eda96884112d111c2c1f15c79",
    "face_landmarker.task": "64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff",
    "selfie_multiclass_256x256.tflite": "c6748b1253a99067ef71f7e26ca71096cd449baefa8f101900ea23016507e0e0",
}


def copy_file(source, destination):
    if source.is_symlink() or not source.is_file():
        raise ValueError("invalid_dependency_source")
    # Static import/link libraries serve compilation, not offline inference.
    if source.suffix.lower() == ".lib":
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source,destination)


def main():
    parser=argparse.ArgumentParser()
    for name in ("base-python","site-packages","weights","modules","destination"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--wheel",action="append",default=[])
    parser.add_argument("--native-file",action="append",type=Path,default=[])
    args=parser.parse_args()
    destination=args.destination.resolve()
    if destination.exists():
        verify(destination,imports=False)
        print(str(destination));return
    args.site_packages=args.site_packages.resolve(strict=True)
    # Requirement parsing is a build dependency; it is never consulted by the installed app.
    sys.path.insert(0,str(args.site_packages))
    from packaging.requirements import Requirement
    from packaging.utils import canonicalize_name
    distributions={canonicalize_name(d.metadata["Name"]):d for d in
        importlib.metadata.distributions(path=[str(args.site_packages)])}
    replacements={"numpy","pillow","opencv-python","opencv-contrib-python","opencv-python-headless"}
    queue=["torch","torchvision","pyfacer","mediapipe","scipy","packaging"]
    selected={}
    while queue:
        name=canonicalize_name(queue.pop())
        if name in selected or name in replacements:continue
        dist=distributions[name];selected[name]=dist
        for raw in dist.requires or []:
            requirement=Requirement(raw)
            if not requirement.marker or requirement.marker.evaluate({"extra":""}):
                queue.append(requirement.name)
    destination.mkdir(parents=True)
    for file in args.base_python.rglob("*"):
        relative=file.relative_to(args.base_python)
        if file.is_file() and "site-packages" not in relative.parts and "__pycache__" not in relative.parts:
            copy_file(file,destination/"python"/relative)
    for file in args.native_file:
        copy_file(file,destination/"python"/file.name)
    packages={}
    for name,dist in selected.items():
        packages[name]=dist.version
        for file in dist.files or []:
            relative=Path(str(file))
            if relative.is_absolute() or ".." in relative.parts or "__pycache__" in relative.parts or relative.name=="direct_url.json":continue
            source=args.site_packages/relative
            if source.is_file():copy_file(source,destination/"python"/"Lib"/"site-packages"/relative)
    for wheel_argument in args.wheel:
        wheel_text,expected=wheel_argument.rsplit("=",1)
        wheel=Path(wheel_text)
        if digest(wheel)!=expected:raise ValueError("pinned_wheel_hash_mismatch")
        with zipfile.ZipFile(wheel) as archive:
            for member in archive.infolist():
                relative=Path(member.filename)
                if relative.is_absolute() or ".." in relative.parts:raise ValueError("unsafe_wheel_path")
                archive.extract(member,destination/"python"/"Lib"/"site-packages")
        packages[wheel.name]=expected
    for name,expected in WEIGHTS.items():
        source=args.weights/name
        if digest(source)!=expected:raise ValueError("model_hash_mismatch")
        copy_file(source,destination/"weights"/name)
    names=[]
    cmake=(args.modules/"local_semantic_runtime_files.cmake").read_text(encoding="utf-8")
    import re
    for name in re.findall(r'tools/ai/([^"\n]+)',cmake):
        copy_file(args.modules/name,destination/"modules"/name);names.append(name)
    files={}
    for file in sorted(destination.rglob("*")):
        if file.is_file():files[file.relative_to(destination).as_posix()]={"size":file.stat().st_size,"sha256":digest(file)}
    manifest={"schema":"orca.offline-portrait-runtime/v1","network":"offline","provider_calls":False,
        "model_download":False,"packages":packages,"files":files,
        "modules":{name:files["modules/"+name] for name in names}}
    (destination/"runtime-manifest.json").write_text(json.dumps(manifest,sort_keys=True,separators=(",",":")),encoding="utf-8")
    verify(destination,imports=False)
    print(str(destination))


if __name__=="__main__":main()

"""Package explicit SDK build outputs; never upload, commit, or discover latest builds."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import ctypes
import zipfile

p = argparse.ArgumentParser()
p.add_argument('--windows-build', type=pathlib.Path, required=True)
p.add_argument('--android-build', type=pathlib.Path, required=True)
p.add_argument('--output', type=pathlib.Path, required=True)
p.add_argument('--windows-slicing-build', type=pathlib.Path)
p.add_argument('--android-slicing-build', type=pathlib.Path)
p.add_argument('--windows-runtime-dir',type=pathlib.Path)
p.add_argument('--cgal-source-archive',type=pathlib.Path)
args = p.parse_args()
if bool(args.windows_slicing_build) != bool(args.android_slicing_build):
    raise SystemExit('Both Slicing build roots must be supplied together')
source = pathlib.Path(__file__).resolve().parents[1]
repo = source.parents[1]
if args.output.exists():
    raise SystemExit('Candidate directory already exists; use a new bundle identity')
files = {
    'bin/orca_model_runner.exe': args.windows_build / 'Release/orca_model_runner.exe',
    'bin/orca_model_sdk_c.dll': args.windows_build / 'Release/orca_model_sdk_c.dll',
    'lib/orca_model_sdk_c.lib': args.windows_build / 'Release/orca_model_sdk_c.lib',
    'android/arm64-v8a/liborca_model_sdk_c.so': args.android_build / 'liborca_model_sdk_c.so',
    'include/orca/model/model_sdk_c.h': source / 'include/orca/model/model_sdk_c.h',
    'CONTRACT.md': source / 'CONTRACT.md',
    'schemas/model-request.schema.json': source/'schemas/model-request.schema.json',
    'schemas/model-response.schema.json': source/'schemas/model-response.schema.json',
    'licenses/OrcaSlicer-LICENSE.txt': repo / 'LICENSE.txt',
}
complete = args.windows_slicing_build is not None
if complete:
    if not args.cgal_source_archive or not args.cgal_source_archive.is_file():
        raise SystemExit('Complete SDK requires the original pinned CGAL source archive')
    expected_cgal='eebd737d9b7f0199647ba5c1f9f6c7a3d0651aacef9c2fd4dedc52da6c25edbe'
    if hashlib.sha256(args.cgal_source_archive.read_bytes()).hexdigest()!=expected_cgal:
        raise SystemExit('CGAL source archive does not match the repository-pinned version/hash')
    files.update({
        'bin/orca_mobile_slicing_sdk_c.dll': args.windows_slicing_build/'Release/orca_mobile_slicing_sdk_c.dll',
        'bin/orca_mobile_slicing_json_gate.exe': args.windows_slicing_build/'Release/orca_mobile_slicing_json_gate.exe',
        'lib/orca_mobile_slicing_sdk_c.lib': args.windows_slicing_build/'Release/orca_mobile_slicing_sdk_c.lib',
        'android/arm64-v8a/liborca_mobile_slicing_sdk_c.so': args.android_slicing_build/'liborca_mobile_slicing_sdk_c.so',
        'include/orca/mobile/slicing_sdk_c.h': repo/'src/orca_mobile_slicing_sdk/include/orca/mobile/slicing_sdk_c.h',
        'dependencies/CGAL-6.2.1.zip': args.cgal_source_archive,
    })
    # Runtime dependencies come only from the two explicitly supplied builds.
    # No external worktree object, global build, or newest-version discovery.
    for directory, pattern, target in [(args.windows_slicing_build, '*.dll', 'bin'),
                                       (args.android_slicing_build, '*.so', 'android/arm64-v8a')]:
        for file in sorted(directory.rglob(pattern)):
            name=f'{target}/{file.name}'
            if name in files and files[name].resolve()!=file.resolve():
                if hashlib.sha256(files[name].read_bytes()).digest()!=hashlib.sha256(file.read_bytes()).digest():
                    raise SystemExit(f'Ambiguous runtime artifact: {name}')
            files[name]=file
    if args.windows_runtime_dir:
        if not args.windows_runtime_dir.is_absolute() or not args.windows_runtime_dir.is_dir():
            raise SystemExit('The explicit Windows runtime directory must exist and be absolute')
        for file in sorted(args.windows_runtime_dir.glob('*.dll')):
            files['bin/'+file.name]=file
for name, file in files.items():
    if not file.is_file():
        raise SystemExit(f'Missing required artifact: {file}')
tracked_diff = subprocess.check_output(['git', '-C', str(repo), 'diff', '--binary', 'HEAD'])
sources = []
for file in sorted(source.rglob('*')):
    if file.is_file() and '__pycache__' not in file.parts:
        sources.append({'path': file.relative_to(repo).as_posix(),
                        'sha256': hashlib.sha256(file.read_bytes()).hexdigest()})
for name in ['src/slic3r/GUI/AI/Model/VertexColorRegionEditor.cpp',
             'src/slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp',
             'src/orca_mobile_slicing_sdk/platform/AndroidSha256Digest.cpp',
             'src/libslic3r/Sha256Digest.hpp']:
    sources.append({'path':name,'sha256':hashlib.sha256((repo/name).read_bytes()).hexdigest()})
if complete:
    tracked=subprocess.check_output(['git','-C',str(repo),'ls-files','-z','mobile-slicing','src/libslic3r',
        'src/orca_mobile_slicing_sdk','src/slic3r/AI','src/slic3r/GUI/AI/Orca','deps_src','deps','cmake','scripts',
        'resources/info/nozzle_incompatibles.json'],text=True).split('\0')
    known={item['path'] for item in sources}
    for name in sorted(filter(None,tracked)):
        file=repo/name
        if name not in known and file.is_file():
            sources.append({'path':name,'sha256':hashlib.sha256(file.read_bytes()).hexdigest()})
snapshot = hashlib.sha256(json.dumps(sources, sort_keys=True).encode()).hexdigest()
args.output.mkdir(parents=True)
manifest = {'schemaVersion': 1, 'sdkVersion': 'orca-model-sdk/1.0.0',
            'contractVersion': '1.0', 'abiVersion': '1.0',
            'bundleId': args.output.name, 'platform': 'windows', 'architecture': 'x64',
            'runner': 'bin/orca_model_runner.exe', 'uncommitted': True,
            'sourceCommit': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
            'sourceSnapshot': snapshot, 'trackedDiffSha256': hashlib.sha256(tracked_diff).hexdigest(),
            'sourceFiles': sources, 'android': {'abi': 'arm64-v8a', 'api': 24, 'ndk': '28.2.13676358'},
            'scope': 'foundation-not-full-P1-P3',
            'knownGaps': ['full-format manufacturing IO adapter', 'advanced repair and multi-plate project operations',
                          'multi-material execution/new Slicing SDK artifact',
                          'actual platform transport supplied by host; real provider calls not tested'],
            'files': []}
for name, file in files.items():
    dest = args.output / name
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(file, dest)
    data = dest.read_bytes()
    manifest['files'].append({'path': name, 'sha256': hashlib.sha256(data).hexdigest(), 'sizeBytes': len(data)})
cache=(args.windows_build/'CMakeCache.txt').read_text(encoding='utf-8')
dependency_root=None
for line in cache.splitlines():
    if line.startswith('ORCA_MODEL_DEPENDENCY_ROOT:PATH='):
        dependency_root=pathlib.Path(line.split('=',1)[1])
if dependency_root:
    for name,relative in {
        'Boost-LICENSE_1_0.txt':'boost-src/LICENSE_1_0.txt',
        'Eigen-COPYING.MPL2':'eigen-src/COPYING.MPL2',
        'Eigen-COPYING.README':'eigen-src/COPYING.README',
        'oneTBB-LICENSE.txt':'tbb-src/LICENSE.txt',
    }.items():
        original=dependency_root/relative
        if not original.is_file():
            raise SystemExit(f'Missing dependency license {original}')
        destination=args.output/'licenses'/name
        destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(original,destination)
        data=destination.read_bytes()
        manifest['files'].append({'path':'licenses/'+name,'sha256':hashlib.sha256(data).hexdigest(),'sizeBytes':len(data)})
license_text = (repo/'deps_src/nlohmann/json.hpp').read_text(encoding='utf-8').split('*/',1)[0] + '*/\n'
license_file = args.output/'licenses/nlohmann-json-LICENSE.MIT'
license_file.parent.mkdir(parents=True,exist_ok=True)
license_file.write_text(license_text,encoding='utf-8')
data=license_file.read_bytes()
manifest['files'].append({'path':'licenses/nlohmann-json-LICENSE.MIT','sha256':hashlib.sha256(data).hexdigest(),'sizeBytes':len(data)})
library=ctypes.CDLL(str((args.windows_build/'Release/orca_model_sdk_c.dll').resolve()))
library.orca_model_capabilities.restype=ctypes.c_char_p
capabilities=json.loads(library.orca_model_capabilities().decode())
capability_file=args.output/'capabilities.json'
capability_file.write_text(json.dumps(capabilities,indent=2),encoding='utf-8')
data=capability_file.read_bytes()
manifest['files'].append({'path':'capabilities.json','sha256':hashlib.sha256(data).hexdigest(),'sizeBytes':len(data)})
manifest['capabilities']='capabilities.json'
if complete:
    def record(name):
        data=(args.output/name).read_bytes()
        manifest['files'].append({'path':name,'sha256':hashlib.sha256(data).hexdigest(),'sizeBytes':len(data)})
    build_configs={}
    for platform,directory in [('windows-x64',args.windows_slicing_build),('android-arm64-v8a',args.android_slicing_build)]:
        values={}
        for line in (directory/'CMakeCache.txt').read_text(encoding='utf-8').splitlines():
            if '=' in line and ':' in line.split('=',1)[0] and not line.startswith('//'):
                key=line.split(':',1)[0]
                if key.startswith(('ORCA_','CMAKE_CXX_COMPILER','CMAKE_CXX_FLAGS','CMAKE_BUILD_TYPE','CMAKE_GENERATOR','ANDROID_')):
                    values[key]=line.split('=',1)[1]
        if values.get('ORCA_MOBILE_REAL_TRIANGULATION')!='ON':
            raise SystemExit('Complete SDK bundle requires real constrained triangulation')
        build_configs[platform]=values
    manifest['toolchain']=build_configs
    manifest['slicingLibrary']='bin/orca_mobile_slicing_sdk_c.dll'
    manifest['scope']='sdk-core-compilation-candidate-not-complete-desktop-acceptance'
    manifest['knownGaps']=['Real provider quality, mobile device behavior and physical printing are not verified',
                           'Texture-to-material mapping requires an explicit verified conversion; source warnings retained',
                           'Only declared supported native 3MF semantics can manufacture; unknown extensions fail explicitly']
    archive=args.output/'source-snapshot.zip'
    with zipfile.ZipFile(archive,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=1) as output:
        for item in sources:
            output.write(repo/item['path'],item['path'])
        output.writestr('tracked-uncommitted.patch',tracked_diff)
        output.writestr('source-files.json',json.dumps(sources,indent=2))
    record('source-snapshot.zip')
    # Include upstream dependency notices verbatim. The manifest makes no
    # redistribution/licensing approval claim beyond retaining these notices.
    license_roots=[repo/'deps_src']
    for line in (args.windows_slicing_build/'CMakeCache.txt').read_text(encoding='utf-8').splitlines():
        if line.startswith('FETCHCONTENT_SOURCE_DIR_') and ':PATH=' in line:
            license_roots.append(pathlib.Path(line.split('=',1)[1]))
        if line.startswith('ORCA_MOBILE_CGAL_INCLUDE:PATH='):
            license_roots.append(pathlib.Path(line.split('=',1)[1]).parent)
    copied=set()
    for root in license_roots:
        for notice in sorted(root.rglob('*')):
            if not notice.is_file() or (notice.name.upper().split('.')[0] not in {'LICENSE','COPYING','COPYRIGHT','NOTICE'}
                and 'LICENSES' not in [part.upper() for part in notice.relative_to(root).parts]):
                continue
            relative=notice.relative_to(root)
            name='licenses/dependencies/'+root.name+'/'+relative.as_posix()
            if name in copied:
                continue
            copied.add(name)
            destination=args.output/name
            destination.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(notice,destination)
            record(name)
(args.output / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps({'bundle': str(args.output.resolve()), 'sourceSnapshot': snapshot,
                  'manifestSha256': hashlib.sha256((args.output/'manifest.json').read_bytes()).hexdigest()}))

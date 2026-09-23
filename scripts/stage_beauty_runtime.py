"""Stage an explicitly supplied offline CPU beauty runtime for internal packaging.

No installation, network access, provider configuration, user data or absolute
path .pth files are carried into the destination. License metadata is retained.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--python-root', type=Path, required=True)
    parser.add_argument('--site-packages', type=Path, action='append', required=True)
    parser.add_argument('--weights', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise SystemExit('Use a new empty runtime staging directory')
    output.mkdir(parents=True, exist_ok=True)
    def ignored(directory, names):
        return [n for n in names if n in {'__pycache__', 'site-packages', 'Scripts', 'pip', '.git'}
                or n.endswith(('.pyc', '.pth', '.lib', '.pdb', '.exp')) or n.startswith('pip-')]
    shutil.copytree(args.python_root, output / 'python', ignore=ignored, dirs_exist_ok=True)
    packages = output / 'python' / 'Lib' / 'site-packages'
    for source in args.site_packages:
        shutil.copytree(source, packages, ignore=ignored, dirs_exist_ok=True)
    weights = output / 'weights'
    weights.mkdir()
    for name in ('mobilenet0.25_Final.pth', 'face_parsing.farl.celebm.main_ema_181500_jit.pt', 'face_landmarker.task'):
        shutil.copy2(args.weights / name, weights / name)
    body = args.weights / 'selfie_multiclass_256x256.tflite'
    if body.is_file():
        if hashlib.sha256(body.read_bytes()).hexdigest() != 'c6748b1253a99067ef71f7e26ca71096cd449baefa8f101900ea23016507e0e0':
            raise SystemExit('Body model checksum mismatch')
        shutil.copy2(body, weights / body.name)
    # -I must still resolve every dependency entirely within the relocated tree.
    probe = '''import json,sys,torch,torchvision,numpy,PIL,facer,mediapipe
from pathlib import Path
root=Path(sys.executable).resolve().parent
modules=[torch,torchvision,numpy,PIL,facer,mediapipe]
assert all(Path(m.__file__).resolve().is_relative_to(root) for m in modules)
print(json.dumps({m.__name__:getattr(m,'__version__','unknown') for m in modules}))'''
    result = subprocess.run([str(output/'python/python.exe'), '-I', '-B', '-c', probe],
                            capture_output=True, text=True)
    if result.returncode:
        (output/'probe-error.log').write_text(result.stderr, encoding='utf-8')
        raise SystemExit('Portable imports failed; see probe-error.log in the staging directory')
    files = []
    for path in sorted(output.rglob('*')):
        if path.is_file() and '__pycache__' not in path.parts:
            files.append({'path': path.relative_to(output).as_posix(), 'bytes': path.stat().st_size,
                          'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    manifest = {'schema': 'orca.beauty-runtime.v1', 'packages': json.loads(result.stdout), 'files': files}
    (output/'runtime-manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps({'status': 'verified', 'files': len(files), 'bytes': sum(f['bytes'] for f in files),
                      'packages': manifest['packages']}))


if __name__ == '__main__':
    main()

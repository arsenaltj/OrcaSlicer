"""GO-only postprocessing proof; no Native runtime load. Not run by tests."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

from adapter import SOURCE, PUBLIC, identity, require, small_json, write


def prove(plan):
    repo, stage, out, evidence = (Path(plan[k]) for k in ('sdkRepository', 'staging', 'output', 'evidence'))
    spec = importlib.util.spec_from_file_location('normal_model_strip', repo / 'src/orca_model_sdk/tools/strip_android_model.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    records = {}
    for kind in ('model', 'slicing'):
        raw = Path(plan['producers'][kind + '-android']['rawSource']['raw']['path'])
        released = out / ('android/arm64-v8a/liborca_' + ('model_sdk_c.so' if kind == 'model' else 'mobile_slicing_sdk_c.so'))
        before, _ = module.runtime_identity(raw)
        after, _ = module.runtime_identity(released)
        require(before == after, 'allocated sections/header/whole PT_LOAD changed: ' + kind)
        require(all(x[7] >= 16384 and x[2] % 16384 == x[3] % 16384 for x in after['loadSegments']), '16K alignment')
        for flags, label in [(['-d'], 'needed'), (['--dyn-syms', '--wide'], 'dynsym')]:
            texts = []
            for path, tag in ((raw, 'raw'), (released, 'release')):
                text = subprocess.check_output([plan['readelf'], *flags, str(path)], text=True)
                (evidence / (kind + '-' + tag + '-' + label + '.txt')).write_text(text, encoding='utf8')
                texts.append(text)
            require(texts[0] == texts[1], 'dynamic table changed: ' + kind + '/' + label)
            if label == 'dynsym':
                names = {x.split()[-1] for x in texts[1].splitlines() if len(x.split()) >= 8
                         and x.split()[4:6] == ['GLOBAL', 'DEFAULT'] and x.split()[6] != 'UND'
                         and x.split()[-1].startswith('orca_')}
                prefix = 'orca_model_' if kind == 'model' else 'orca_mobile_slicing_'
                require(names == {prefix + n for n in PUBLIC[kind]}, 'Android public ABI')
        records[kind] = dict(raw=identity(raw), release=identity(released), fullRuntimeIdentity=after)
    binding = small_json(stage / 'model-android/approved-tbb-binding.json')
    require(identity(out / 'android/arm64-v8a/libtbb.so')['sha256'] == identity(binding['runtimePath'])['sha256'], 'approved TBB changed')
    for released in (out / 'android/arm64-v8a').glob('*.so'):
        if released.name in ('libtbb.so', 'liborca_model_sdk_c.so', 'liborca_mobile_slicing_sdk_c.so'):
            continue
        raw = stage / 'slicing-android' / released.name
        require(raw.is_file(), 'runtime absent from explicit staged closure')
        before, _ = module.runtime_identity(raw)
        after, _ = module.runtime_identity(released)
        require(before == after, 'runtime allocated/PT_LOAD changed: ' + released.name)
        require(all(x[7] >= 16384 and x[2] % 16384 == x[3] % 16384 for x in after['loadSegments']), 'runtime16K')
        records[released.name] = dict(raw=identity(raw), release=identity(released), fullRuntimeIdentity=after)
    write(evidence / 'android-static-proof.json', dict(source=SOURCE, records=records,
        approvedTbb=identity(binding['runtimePath']), runtimeExecuted=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--binding', required=True)
    prove(small_json(parser.parse_args().binding))

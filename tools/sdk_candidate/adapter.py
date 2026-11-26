"""External b5f candidate adapter. Import/prepare never executes native tools.

Execution requires a separately reviewed complete binding and exact root GO.
The tool checkout is not the SDK source checkout. No compile fallback exists.
"""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile

SOURCE = 'b5f078f52a8db9bb8eb3b347b155679c852d5c32'
TREE = 'cd7a99640b2c396917176e1e7e9ee1e356b6915a'
STAGES = ('model-windows', 'model-android', 'slicing-windows', 'slicing-android')
IO_MONITOR = lambda: None  # Installed only after explicit execution GO.
PUBLIC = {
    'model': ['version', 'capabilities', 'create', 'execute', 'cancel', 'destroy',
              'free_string', 'set_platform_transport', 'set_slicing_backend'],
    'slicing': ['version', 'capabilities', 'create', 'execute', 'cancel', 'destroy', 'free_string'],
}


def require(test, message):
    if not test:
        raise ValueError(message)


def identity(path):
    path = Path(path)
    require(path.is_file() and not path.is_symlink(), 'regular file required: ' + str(path))
    with path.open('rb') as stream:
        digestor = hashlib.sha256()
        while block := stream.read(8 * 1024 * 1024):
            IO_MONITOR()
            digestor.update(block)
        digest = digestor.hexdigest()
    return dict(path=str(path), bytes=path.stat().st_size, sha256=digest)


def verify(item):
    require(item is not None, 'unbound file')
    actual = identity(item['path'])
    require(actual['sha256'] == item['sha256'] and actual['bytes'] == item['bytes'],
            'file identity changed: ' + str(item['path']))
    return Path(item['path'])


def small_json(path):
    require(Path(path).stat().st_size <= 4 * 1024 * 1024, 'not a small metadata file')
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def dependency_json(ref):
    """Only the exact pinned canonical dependency manifest may exceed 4MiB.

    Original execution records still never enter any JSON loader. The 16MiB
    ceiling covers the existing 10,464,796-byte canonical input without making
    the compact receipt limit apply to an unrelated dependency-file index.
    """
    require(isinstance(ref, dict) and type(ref.get('bytes')) is int
            and 0 < ref['bytes'] <= 16 * 1024 * 1024, 'dependency manifest size bound')
    require(Path(ref['path']).name not in ('execution.json', 'actual-compiler-inputs.json'),
            'not a canonical dependency manifest')
    path = verify(ref)
    with path.open('rb') as stream:
        payload = stream.read(ref['bytes'] + 1)
    require(len(payload) == ref['bytes'] and hashlib.sha256(payload).hexdigest() == ref['sha256'],
            'dependency manifest changed during bounded read')
    return json.loads(payload.decode('utf-8-sig'))


def write(path, value):
    with Path(path).open('x', encoding='utf8') as out:
        json.dump(value, out, indent=2, ensure_ascii=False)
        out.write('\n')


def terminal_ok(stage, terminal, raw):
    require(terminal['source'] == raw['source'] == SOURCE, 'wrong SDK source')
    require(raw['cleanBuild'] is True and raw['copiedPredecessorObjects'] == 0
            and raw['priorObjectsReused'] is False, 'not a clean producer')
    require(terminal.get('actualExitCode', terminal.get('actualExit')) == 0, 'terminal failed')
    require(terminal.get('sourceBindings', 0) > 0, 'missing source closure')
    require(terminal.get('sdkExecuted') is False, 'unexpected SDK execution')
    if stage == 'model-android':
        records = terminal['records']
        require([r['name'] for r in records] == ['configure', 'actual-ninja-commands', 'build', 'actual-ninja-deps'],
                'incomplete Android normal command family')
        for r in records:
            require(r['exitCode'] == 0 and r['state'] == 'returned'
                    and r['monitorErrors'] == 0 and r['runtimeViolations'] == 0, 'bad resource record')
    elif stage == 'slicing-android':
        require(terminal.get('state') == 'SCOPED_SOURCE_RECOVERY_PASSED'
                and terminal.get('ownerProcessAbsent') is True, 'unclosed recovery')
    elif stage == 'model-windows':
        require('records' not in terminal, 'wrong Windows schema')
        require(terminal.get('fullSourceProvenancePassed') is True
                and terminal.get('ownerPid16320Absent') is True, 'unclosed Model recovery')
    else:
        require(stage == 'slicing-windows' and 'records' not in terminal, 'wrong Windows schema')
        require(terminal.get('buildAndSourcePassed') is True
                and terminal.get('resourceReturned') is True, 'unbound Windows normal success')


def prepare_receipt(stage, directory, seal_name):
    """Only read compact terminal/raw/seal. Preserve every sealed field/index.

    Large records are NOT parsed or hashed here. Future execution streams them.
    Recovery origin retains the original failed attempt rather than upgrading it.
    """
    directory = Path(directory)
    terminal = small_json(directory / 'actual-terminal.json')
    raw = small_json(directory / 'raw-source.json')
    terminal_ok(stage, terminal, raw)
    seal = small_json(directory / seal_name)
    refs = seal.get('files', seal.get('refs'))
    require(isinstance(refs, list) and refs, 'missing full sealed index')
    indexed = {str(Path(x['path']).resolve()): x for x in refs}
    require(len(indexed) == len(refs), 'duplicate seal path')
    for name in ('actual-terminal.json', 'raw-source.json', 'actual-compiler-inputs.json',
                 'actual-source-command-flags.json', 'new-object-inventory.json'):
        require(str((directory / name).resolve()) in indexed, 'incomplete source index: ' + name)
    for name in ('actual-terminal.json', 'raw-source.json'):
        verify(indexed[str((directory / name).resolve())])
    return dict(stage=stage, terminal=terminal, rawSource=raw,
                seal=identity(directory / seal_name), fullIndex=seal,
                originalRecords=(small_json(directory / 'recovery-origin.json').get('originalRecords', {})
                                 if (directory / 'recovery-origin.json').exists() else {}),
                bindingStatus='SOURCE_RECEIPT_ONLY_NOT_EXECUTION_AUTHORITY')


def relative(name):
    p = PurePosixPath(name)
    require(not p.is_absolute() and name == p.as_posix() and '..' not in p.parts
            and ':' not in name and '\\' not in name and name, 'unsafe relative name')
    return name


def validate_binding(plan):
    require(plan['source'] == SOURCE and plan['tree'] == TREE, 'wrong SDK source/tree')
    require(set(plan['producers']) == set(STAGES), 'four producer stages required')
    for stage in STAGES:
        item = plan['producers'][stage]
        require(item is not None, 'unbound stage: ' + stage)
        terminal_ok(stage, item['terminal'], item['rawSource'])
        require(bool(item.get('resourceProof')), 'unbound resource/full execution proof')
        require(bool(item.get('sourceProof')), 'unbound complete source proof')
    roots = [Path(plan[k]).resolve() for k in ('staging', 'output', 'evidence', 'debug')]
    require(len(set(roots)) == 4, 'overlapping fresh roots')
    for a in roots:
        require(not a.exists(), 'fresh output required: ' + str(a))
        require(all(a == b or not a.is_relative_to(b) for b in roots), 'nested output roots')
        for item in plan['producers'].values():
            source = Path(item['rawSource']['buildPath']).resolve().parent
            require(not a.is_relative_to(source) and not source.is_relative_to(a), 'producer/output overlap')
    require(Path(plan['output']).name == 'sdk-resource-complete-' + SOURCE[:12] + '-r1', 'wrong namespace')
    require(plan['functionalQualification'] is None, 'no inherited functional qualification')
    require(plan['postprocessProof'] is not None, 'unbound static proof validator')
    require(plan['toolSource'] != SOURCE and re.fullmatch('[0-9a-f]{40}', plan['toolSource']), 'tool/source identities must be separate')
    inputs = {x['destination']: x['source'] for x in plan['stageInputs']}
    require(len(inputs) == len(plan['stageInputs']), 'duplicate stage destination')
    for name in inputs:
        relative(name)
        require(not name.lower().endswith(('.obj', '.o', '.pch')), 'object staging forbidden')
    for stage, item in plan['producers'].items():
        raw = item['rawSource']['raw']
        name = Path(raw['path']).name
        expected = {'model-windows': 'orca_model_sdk_c.dll', 'slicing-windows': 'orca_mobile_slicing_sdk_c.dll',
                    'model-android': 'liborca_model_sdk_c.so', 'slicing-android': 'liborca_mobile_slicing_sdk_c.so'}
        require(name == expected[stage], 'wrong canonical stage raw')
        dest = stage + ('/Release/' if stage.endswith('windows') else '/') + name
        require(inputs.get(dest) == raw, 'stage SDK must come from exact producer raw')
    import contracts
    contracts.check(plan, sys.modules[__name__])
    contracts.owner_protocol(plan, sys.modules[__name__])
    return roots


def verify_indexes(plan):
    """Stream full index files including GB execution records; no schema conversion."""
    for item in plan['producers'].values():
        verify(item['seal'])
        require(small_json(item['seal']['path']) == item['fullIndex'], 'fullIndex differs from original seal')
        for ref in item['fullIndex'].get('files', item['fullIndex'].get('refs', [])):
            verify(ref)
        for ref in item['resourceProof'] + item['sourceProof']:
            verify(ref)
        for ref in item.get('originalRecords', {}).values():
            verify(ref)
        raw = item['rawSource']
        verify(raw['raw'])
        verify(dict(raw['raw'], path=raw['buildPath']))


def copy_inputs(plan, monitor=lambda: None):
    stage = Path(plan['staging'])
    stage.mkdir(parents=True)
    journal = stage / 'copied-inputs.jsonl'
    seen = set()
    with journal.open('x', encoding='utf8') as log:
        for item in plan['stageInputs']:
            name = relative(item['destination'])
            require(name not in seen and not name.lower().endswith(('.obj', '.o', '.pch')),
                    'duplicate/object staging input')
            seen.add(name)
            src = verify(item['source'])
            dst = stage / name
            dst.parent.mkdir(parents=True, exist_ok=True)
            with src.open('rb') as inp, dst.open('xb') as out:
                while block := inp.read(8 * 1024 * 1024):
                    monitor()
                    out.write(block)
            check = identity(dst)
            require(check['sha256'] == item['source']['sha256'], 'copy bytes changed')
            log.write(json.dumps(dict(source=item['source'], saved=check)) + '\n')
            log.flush()
            os.fsync(log.fileno())
    # Caches remain the exact original producer bytes; never patch paths.
    for stage_name in STAGES:
        require(stage_name + '/CMakeCache.txt' in seen, 'missing producer original cache')


def parse_exports(text, kind):
    table = re.findall(r'Export \{\s+Ordinal: (\d+)\s+Name: ([^\n]+)\s+RVA: ([^\n]+)', text)
    require(table, 'missing full export table')
    prefix = 'orca_model_' if kind == 'model' else 'orca_mobile_slicing_'
    names = re.findall(r'^  Name: (.+)$', text, re.M)
    require(len(table) == len(names) == len(set(names)), 'incomplete or duplicate export table')
    public = [(int(o), n.strip()) for o, n, _ in table if n.strip().startswith('orca_')]
    base = 1 if kind == 'model' else 433
    expected = {prefix + n for n in PUBLIC[kind]}
    require(len(public) == len(expected) and {n for _, n in public} == expected,
            'public exports differ')
    # Stable public ordinals are alphabetical, as in the original PE contract.
    require(sorted(public) == list(zip(range(base, base + len(expected)), sorted(expected))),
            'public ordinals differ')
    return table


def select_tbb(plan, command):
    stage, evidence = Path(plan['staging']), Path(plan['evidence'])
    tables, names, imports = {}, {}, {}
    for kind in ('model', 'slicing'):
        build = stage / (kind + '-windows')
        raw = build / 'raw-tbb/tbb12.dll'
        text = command([plan['readobj'], '--coff-exports', str(raw)], kind + '-tbb-exports')
        tables[kind] = re.findall(r'Export \{\s+Ordinal: (\d+)\s+Name: ([^\n]+)\s+RVA: ([^\n]+)', text)
        names[kind] = {n for _, n, _ in tables[kind]}
        require(names[kind], 'empty TBB exports')
        sdk = build / 'Release' / ('orca_model_sdk_c.dll' if kind == 'model' else 'orca_mobile_slicing_sdk_c.dll')
        parse_exports(command([plan['readobj'], '--coff-exports', str(sdk)], kind + '-sdk-exports'), kind)
        text = command([plan['readobj'], '--coff-imports', str(sdk)], kind + '-sdk-imports')
        current, symbols = None, set()
        for line in text.splitlines():
            match = re.match(r'  Name: (.+)', line)
            if match:
                current = match[1].lower()
            match = re.match(r'  Symbol: (.+?)(?: \([0-9]+\))?$', line)
            if match and current == 'tbb12.dll':
                symbols.add(match[1])
        require(symbols, 'missing real TBB imports')
        imports[kind] = sorted(symbols)
    require(names['model'] == names['slicing'], 'TBB exports incompatible')
    require(all(set(x) <= names['slicing'] for x in imports.values()), 'unresolved TBB imports')
    caches = {}
    for kind in ('model', 'slicing'):
        cache = stage / (kind + '-windows/CMakeCache.txt')
        caches[kind] = {line.split(':', 1)[0]: line.split('=', 1)[1]
                        for line in cache.read_text(encoding='utf8').splitlines()
                        if not line.startswith(('#', '//')) and ':' in line and '=' in line}
    require((Path(caches['model']['ORCA_MODEL_DEPENDENCY_ROOT']) / 'tbb-src').resolve()
            == Path(caches['slicing']['FETCHCONTENT_SOURCE_DIR_TBB']).resolve()
            == Path(plan['tbbSource']).resolve(), 'TBB source/cache mismatch')
    selected = stage / 'slicing-windows/raw-tbb/tbb12.dll'
    for kind in ('model', 'slicing'):
        shutil.copyfile(selected, stage / (kind + '-windows/Release/tbb12.dll'))
    write(evidence / 'tbb-selection.json', dict(fullTables=tables, imports=imports,
                                               selected=identity(selected), source=SOURCE))


def argv_for(plan):
    s = Path(plan['staging'])
    repo = Path(plan['sdkRepository'])
    return [plan['python'], str(repo / 'src/orca_model_sdk/tools/package_sdk.py'),
            '--windows-build', str(s / 'model-windows'), '--android-build', str(s / 'model-android'),
            '--windows-slicing-build', str(s / 'slicing-windows'),
            '--android-slicing-build', str(s / 'slicing-android/package-arm64-v8a'),
            '--repair-tbb-source', plan['tbbSource'], '--cgal-source-archive', plan['cgalArchive'],
            '--dependency-input-manifest', plan['dependencyManifest'],
            '--android-strip-tool', plan['strip'], '--android-debug-output', plan['debug'],
            '--output', plan['output']]


def stream_copy(source, destination):
    with Path(source).open('rb') as inp, Path(destination).open('xb') as out:
        while block := inp.read(8 * 1024 * 1024):
            IO_MONITOR()
            out.write(block)


def zip_copy(z, source, name):
    with Path(source).open('rb') as inp, z.open(name, 'w', force_zip64=True) as out:
        while block := inp.read(8 * 1024 * 1024):
            IO_MONITOR()
            out.write(block)


def finish_manifest(plan):
    out, evidence, repo = (Path(plan[k]) for k in ('output', 'evidence', 'sdkRepository'))
    manifest_path = out / 'manifest.json'
    manifest = small_json(manifest_path)
    require(manifest['sourceCommit'] == manifest['packagingSourceCommit'] == SOURCE, 'packager source drift')
    stream_copy(manifest_path, evidence / 'upstream-manifest.json')
    stream_copy(out / 'source-snapshot.zip', evidence / 'upstream-source-snapshot.zip')
    # Explicit source list is derived from current frozen SDK git, not tool git.
    source_files = {i['path']: i for i in manifest['sourceFiles']}
    permanent = subprocess.check_output(['git', '-C', str(repo), 'ls-files', '-z', 'src',
        'mobile-slicing', 'tests', 'docs/coordination/current-development.md',
        'Docs/plans/2026-09-25-product-convergence-and-lightweight-plan.md'],env=dict(os.environ),cwd=repo).decode('utf8').split('\0')
    permanent = sorted(set(filter(None, permanent)))
    require('mobile-slicing/tests/honor_multihead_thermal_test.cpp' in permanent,
            'missing permanent thermal source')
    for name in permanent:
        relative(name)
        source_files[name] = dict(path=name, sha256=identity(repo / name)['sha256'])
    manifest['sourceFiles'] = sorted(source_files.values(), key=lambda x: x['path'])
    with zipfile.ZipFile(evidence / 'upstream-source-snapshot.zip') as old, zipfile.ZipFile(out / 'source-snapshot.zip', 'w', zipfile.ZIP_DEFLATED) as z:
        old_names = set(old.namelist())
        require(len(old_names) == len(old.namelist()), 'duplicate source ZIP names')
        for info in old.infolist():
            if info.filename != 'source-files.json':
                with old.open(info) as inp, z.open(info, 'w', force_zip64=True) as dest:
                    while block := inp.read(8 * 1024 * 1024):
                        IO_MONITOR()
                        dest.write(block)
        for name in permanent:
            if name not in old_names:
                zip_copy(z, repo / name, name)
        z.writestr('source-files.json', json.dumps(manifest['sourceFiles'], indent=2))
    for i in manifest['sourceFiles']:
        require(identity(repo / relative(i['path']))['sha256'] == i['sha256'], 'source mismatch')
    provenance = out / 'provenance'
    provenance.mkdir(exist_ok=False)
    write(provenance / 'binding.json', plan)
    # Full sealed indexes and large original executions preserved as sidecars.
    for stage, item in plan['producers'].items():
        refs = item['fullIndex'].get('files', item['fullIndex'].get('refs', []))
        for index, ref in enumerate(refs + item['resourceProof'] + item['sourceProof']
                                    + list(item.get('originalRecords', {}).values())):
            dest = provenance / stage / (str(index) + '-' + Path(ref['path']).name)
            dest.parent.mkdir(exist_ok=True)
            stream_copy(verify(ref), dest)
        stream_copy(verify(item['seal']), provenance / stage / 'full-original-seal.json')
    for file in evidence.iterdir():
        if file.is_file():
            stream_copy(file, provenance / file.name)
    manifest['sourceSnapshot'] = hashlib.sha256(json.dumps(manifest['sourceFiles'], sort_keys=True).encode()).hexdigest()
    # Upstream literal is preserved above. Caller has just checked actual clean
    # Git/source identity and all four sealed producer closures before assembly.
    manifest.update(uncommitted=False, sourceStatus='clean', buildSourceCommit=SOURCE)
    manifest['artifactAssembly'] = dict(sdkSource=SOURCE, toolSource=plan['toolSource'],
        fullSourceIndexesPreserved=True, cleanProducers=True, copiedPredecessorObjects=0,
        functionalQualification=None, pending=['independent-source-artifact-review', 'new-source-limited-nonGLB-HostQA'],
        stopped=['GLBpre3post3', 'CPU936', 'sixGLB-resource-loops', 'held', 'refused-collector'])
    # Include each actual file exactly once; no fixed 263/3081 counts.
    manifest['files'] = []
    for file in sorted(out.rglob('*')):
        if file.is_file() and file != manifest_path:
            value = identity(file)
            manifest['files'].append(dict(path=file.relative_to(out).as_posix(), sha256=value['sha256'], sizeBytes=value['bytes']))
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding='utf8')
    with zipfile.ZipFile(out / 'source-snapshot.zip') as z:
        require(small_json(manifest_path)['sourceFiles'] == json.loads(z.read('source-files.json')), 'source list mismatch')
        for i in manifest['sourceFiles']:
            require(hashlib.sha256(z.read(i['path'])).hexdigest() == i['sha256'], 'archived source changed')
    archive = evidence / (out.name + '.zip')
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED) as z:
        for file in sorted(out.rglob('*')):
            if file.is_file():
                zip_copy(z, file, file.relative_to(out).as_posix())
    return dict(state='CANDIDATE_NOT_HOST_QUALIFIED',
        manifest=identity(manifest_path), archive=identity(archive), payloadCount=len(manifest['files']),
        sourceCount=len(manifest['sourceFiles']))


def publish_result(evidence, result, final_checks, save):
    """Only a fully closed operation publishes the atomic completion receipt."""
    pending = Path(evidence) / 'candidate-result.pending.json'
    final = Path(evidence) / 'candidate-result.json'
    require(not final.exists(), 'completion receipt already exists')
    write(pending, result)
    final_checks()
    save()
    os.replace(pending, final)


def execute(plan, go, plan_sha):
    global IO_MONITOR
    require(go['source'] == SOURCE and go['planSha256'] == plan_sha
            and go['artifactOnlyAuthorized'] is True
            and go['allowModelCapabilitiesLoad'] is True, 'separate explicit artifact GO required')
    require(identity(go['planPath'])['sha256'] == plan_sha, 'GO plan path mismatch')
    require(Path(plan['bindingPath']).resolve() == Path(go['planPath']).resolve(), 'artifact binding argv differs')
    validate_binding(plan)
    import contracts
    contracts.go_closure(plan, go, sys.modules[__name__])
    lifecycle = contracts.normal_lifecycle(plan, sys.modules[__name__], go)
    required_tools = {str(Path(plan['ownerDirectory']) / n) for n in
                      ('build_resources.py', 'kernel_job.py', 'qa_owner_safety.py')}
    require(required_tools <= {x['path'] for x in plan['toolFiles']}, 'unpinned owner imports')
    for ref in plan['toolFiles'] + go['closure']:
        verify(ref)
    # No import of owner (and hence no monitor) until the exact execution gate.
    sys.path.insert(0, plan['ownerDirectory'])
    from build_resources import gate, sample, heavy
    from artifact_owner import run_owned, admit_cancel, commands
    evidence = Path(plan['evidence'])
    cancel = Path(go['cancel'])
    admit_cancel(cancel)
    state = dict(source=SOURCE, commands=[], state='started')
    published = False
    samples = []
    gate(samples, lambda: None)
    repo = Path(plan['sdkRepository'])
    base_env=dict(os.environ)
    def source_check():
        require(subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True,env=base_env,cwd=repo).strip() == SOURCE, 'SDK source changed')
        require(subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD^{tree}'], text=True,env=base_env,cwd=repo).strip() == TREE, 'SDK tree changed')
        require(not subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain'], text=True,env=base_env,cwd=repo).strip(), 'SDK dirty')
    source_check()
    evidence.mkdir(parents=True)
    def save():
        (evidence / 'execution.json').write_text(json.dumps(state, indent=2), encoding='utf8')
    last = [0.0]
    def monitor(force=False):
        admit_cancel(cancel)
        if force or time.monotonic() - last[0] >= 5:
            x = sample()
            state.setdefault('ioSamples', []).append(x)
            last[0] = time.monotonic()
            save()
            require(x['freeKiB'] > 3 * 1024 ** 2 and not any(heavy(p) for p in x['processes']),
                    'IO resource first failure')
    def command(argv, label, env=None):
        source_check()
        admit_cancel(cancel)
        step = dict(label=label, argv=argv)
        state['commands'].append(step)
        gate(step.setdefault('admission', []), save)
        child_env=dict(base_env)
        if env:child_env.update(env)
        run_owned(argv, evidence / (label + '.log'), step, save, cancel, lambda: None, mode='artifact',
                  permitted_commands=commands(plan),
                  permitted_images=[plan[k] for k in ('python','node','readobj','readelf','strip','git')],env=child_env,cwd=repo,
                  timeout=lifecycle['commandTimeoutSeconds'], lifecycle_profile=lifecycle['profile'], package_plan=plan)
        return (evidence / (label + '.log')).read_text(encoding='utf8')
    try:
        IO_MONITOR = monitor
        verify_indexes(plan)
        copy_inputs(plan, monitor)
        select_tbb(plan, command)
        s = Path(plan['staging'])
        # Literal --verify-only is mandatory. The original -j2 branch is unreachable.
        command([plan['node'], str(repo / 'mobile-slicing/build_android_arm64.cjs'), '--verify-only'],
                'slicing-verify-only', dict(ORCA_SLICING_BUILD_DIR=str(s / 'slicing-android'),
                    ORCA_SLICING_EVIDENCE=str(evidence / 'android'), ANDROID_NDK=plan['ndk']))
        command(argv_for(plan), 'canonical-packager')
        # Existing precise ELF validator is an explicit pinned interface, never a successful default.
        proof = plan['postprocessProof']
        command([plan['python'], str(verify(proof)), '--binding', go['planPath']], 'static-postprocess-proof')
        verify_indexes(plan)
        source_check()
        result = finish_manifest(plan)
        verify_indexes(plan)
        source_check()
        monitor(force=True)
        state['state'] = 'candidate_complete_independent_review_pending'
        save()
        publish_result(evidence, result, lambda: (source_check(), monitor(force=True)), save)
        published = True
    except BaseException as exc:
        state['state'] = 'failed_partial_no_retry'
        state['error'] = repr(exc)
        raise
    finally:
        IO_MONITOR = lambda: None
        if not published:
            save()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binding', required=True)
    p.add_argument('--binding-sha256', required=True)
    p.add_argument('--go')
    p.add_argument('--go-sha256')
    args = p.parse_args()
    require(identity(args.binding)['sha256'] == args.binding_sha256, 'binding hash mismatch')
    plan = small_json(args.binding)
    if args.go:
        require(identity(args.go)['sha256'] == args.go_sha256, 'GO hash mismatch')
        execute(plan, small_json(args.go), args.binding_sha256)
    else:
        validate_binding(plan)
        print('STATIC_COMPLETE_BINDING_ONLY_NO_EXECUTION')


if __name__ == '__main__':
    main()

"""Fail-closed preflight. Only small receipts/tool identities; no native/CIM."""
from pathlib import Path
import json
import subprocess
import re


def check(plan, api):
    need, pin, load = api.require, api.verify, api.small_json
    def shape(ref):
        need(isinstance(ref, dict) and isinstance(ref.get('path'), str)
             and Path(ref['path']).is_absolute() and type(ref.get('bytes')) is int
             and ref['bytes'] >= 0 and isinstance(ref.get('sha256'), str)
             and re.fullmatch('[0-9a-f]{64}', ref['sha256']), 'malformed file pin')
    for item in plan['producers'].values():
        for ref in [item['seal'], item['rawSource']['raw']] + item['resourceProof'] + item['sourceProof']:
            shape(ref)
        for ref in item['fullIndex'].get('files', item['fullIndex'].get('refs', [])):
            shape(ref)
    for ref in plan['toolFiles'] + [x['source'] for x in plan['stageInputs']]:
        shape(ref)
    def refs(index):
        value = index.get('files', index.get('refs'))
        need(isinstance(value, list) and value, 'empty full index')
        paths = [str(Path(x['path']).resolve()) for x in value]
        need(len(set(paths)) == len(paths), 'duplicate full index')
        return {str(Path(x['path']).resolve()): x for x in value}
    for stage, item in plan['producers'].items():
        sealed = load(pin(item['seal']))
        need(sealed == item['fullIndex'], 'fullIndex differs from original seal')
        graph = refs(sealed)
        origins = [x for x in graph.values() if Path(x['path']).name == 'recovery-origin.json']
        actual_originals = load(pin(origins[0])).get('originalRecords', {}) if origins else {}
        need(item.get('originalRecords', {}) == actual_originals, 'original record graph differs')
        for ref in item.get('originalRecords', {}).values():
            key = str(Path(ref['path']).resolve())
            need(key not in graph or graph[key] == ref, 'conflicting original record identity')
            graph[key] = ref
        def member(ref):
            need(isinstance(ref, dict) and graph.get(str(Path(ref['path']).resolve())) == ref,
                 'proof not in exact original graph')
            return ref
        for name in ('actual-terminal.json', 'raw-source.json', 'actual-compiler-inputs.json',
                     'actual-source-command-flags.json', 'new-object-inventory.json'):
            matches = [x for x in refs(sealed).values() if Path(x['path']).name == name]
            need(len(matches) == 1, 'missing/ambiguous required source member: ' + name)
            if name == 'actual-terminal.json':
                need(load(pin(matches[0])) == item['terminal'], 'terminal not exact sealed bytes')
            if name == 'raw-source.json':
                need(load(pin(matches[0])) == item['rawSource'], 'raw-source not exact sealed bytes')
        # Proof summaries are separate reviewed metadata; their origin pointers
        # must equal the actual original graph, not merely hash arbitrary files.
        need(len(item['resourceProof']) == len(item['sourceProof']) == 1, 'one typed proof per role')
        summaries = [load(pin(item[key][0])) for key in ('resourceProof', 'sourceProof')]
        for role, summary in zip(('resource', 'source'), summaries):
            need(summary['schema'] == 'sdk-producer-' + role + '-v1'
                 and summary['source'] == api.SOURCE and summary['stage'] == stage, 'wrong proof schema/stage')
            need(summary['seal'] == item['seal'] and summary['rawSource'] == item['rawSource'], 'proof source/raw/seal mismatch')
        resource, source = summaries
        if stage == 'model-android':
            need(item['terminal'].get('owner29816Absent') is True
                 and item['terminal'].get('sourceClean') is True, 'unclosed Model Android owner/source')
        original = member(resource['summary'])
        record = member(resource['execution'])
        need(Path(record['path']).name == 'execution.json', 'wrong full execution record')
        data = load(pin(original))
        need(data['source'] == api.SOURCE, 'wrong resource source')
        # Original normal producers write the resource summary and execution
        # into the same unique output directory. Resolve the association from
        # the immutable original seal/origin index, not from typed pointers or
        # a basename search. This also supplies size/path for legacy summaries
        # that only record an execution SHA (or contain command metadata only).
        canonical = str((Path(original['path']).resolve().parent / 'execution.json').resolve())
        need(canonical in graph and graph[canonical] == record,
             'resource summary does not bind this exact original execution path/size/SHA')
        for field in ('executionSha256', 'originalRecordSha256'):
            if field in data:
                need(data[field] == record['sha256'], 'summary execution SHA differs')
        for field in ('executionBytes', 'originalRecordBytes'):
            if field in data:
                need(data[field] == record['bytes'], 'summary execution size differs')
        if stage == 'model-windows':
            need(data['buildState'] == 'returned' and data['ownerPidsRemaining'] == 0
                 and data['buildExitCode'] == data['configureExitCode'] == 0
                 and data['monitorErrors'] == data['runtimeViolations'] == 0
                 and data['executionSha256'] == record['sha256'], 'failed original Windows resources')
            # Source-only original wrapper failure remains recorded, never greened.
        else:
            commands = data.get('records') if stage == 'model-android' else data.get('commands')
            need(isinstance(commands, list) and [x['name'] for x in commands] ==
                 (['configure', 'actual-ninja-commands', 'build', 'actual-ninja-deps']
                  if stage.endswith('android') else ['configure', 'build']), 'wrong resource command schema')
            for row in commands:
                need(row['state'] == 'returned' and row['exitCode'] == 0
                     and not row['monitorErrors'] and not row['runtimeViolations']
                     and row.get('cancelReason') is None, 'failed or unreturned resource command')
            if stage == 'slicing-android':
                need(data['originalRecordSha256'] == record['sha256'] and data['originalRecordBytes'] == record['bytes'], 'wrong original record')
        need(source['terminal'] == next(x for x in refs(sealed).values()
                                       if Path(x['path']).name == 'actual-terminal.json'), 'source proof terminal differs')
        need(source['execution'] == record, 'source/resource record mismatch')
        need(source['cache'] == resource['cache'], 'source/resource cache mismatch')
        cache = source['cache']
        need(Path(cache['path']).name == 'CMakeCache.txt', 'wrong cache')
        source_inputs = load(pin(next(x for x in refs(sealed).values()
                                     if Path(x['path']).name == 'actual-compiler-inputs.json')))
        def contains(value, target):
            if isinstance(value, dict):
                if all(value.get(k) == v for k, v in target.items()):
                    return True
                return any(contains(x, target) for x in value.values())
            return isinstance(value, list) and any(contains(x, target) for x in value)
        need(contains(source_inputs, cache), 'cache not bound by actual source input closure')
        item_cache = next((x['source'] for x in plan['stageInputs']
                           if x['destination'] == stage + '/CMakeCache.txt'), None)
        need(item_cache == cache, 'staged cache differs from actual source')
        layout = {x['destination']: x['source'] for x in plan['stageInputs']
                  if x['destination'].startswith(stage + '/')}
        need(source['canonicalInputs'] == layout, 'canonical source layout differs')
        runtimes = source['runtimeFiles']
        actual_runtimes = {name: ref for name, ref in layout.items()
                           if name.lower().endswith(('.dll', '.so'))}
        need(runtimes == actual_runtimes and runtimes, 'runtime closure missing or different')

    repo = Path(plan['sdkRepository']).resolve()
    tool_repo = Path(__file__).resolve().parents[2]
    need(subprocess.check_output(['git', '-C', str(tool_repo), 'rev-parse', 'HEAD'], text=True).strip()
         == plan['toolSource'], 'running tool commit differs')
    need(not subprocess.check_output(['git', '-C', str(tool_repo), 'status', '--porcelain'], text=True).strip(), 'tool checkout dirty')
    required = [Path(__file__), Path(api.__file__), Path(__file__).with_name('static_proof.py')]
    required += [repo / name for name in ('src/orca_model_sdk/tools/package_sdk.py',
        'src/orca_model_sdk/tools/strip_android_model.py', 'src/orca_model_sdk/repair_core/verify_approved_tbb.py',
        'src/orca_model_sdk/repair_core/approved_tbb.json', 'mobile-slicing/build_android_arm64.cjs')]
    required += [Path(plan['ownerDirectory']) / name for name in
                 ('build_resources.py', 'qa_owner_safety.py')]
    required += [Path(plan[name]) for name in ('python', 'node', 'readobj', 'readelf', 'strip', 'cgalArchive', 'dependencyManifest')]
    tools = {str(Path(x['path']).resolve()): x for x in plan['toolFiles']}
    need(len(tools) == len(plan['toolFiles']), 'duplicate tool pins')
    for path in required:
        need(str(path.resolve()) in tools, 'missing required tool/import/binary pin: ' + str(path))
    for item in tools.values():
        pin(item)
    need(plan['postprocessProof'] == tools[str(Path(__file__).with_name('static_proof.py').resolve())], 'wrong static proof implementation')
    deps = api.dependency_json(tools[str(Path(plan['dependencyManifest']).resolve())])
    need(isinstance(deps, list) and deps, 'empty dependencies')
    for entry in deps:
        need(Path(entry['path']).is_absolute() and entry['files'], 'unbound dependency tree')
    need((Path(plan['tbbSource']) / 'LICENSE.txt').is_file(), 'missing fixed TBB source')
    inputs = {x['destination']: x['source'] for x in plan['stageInputs']}
    required_layout = {
        'model-windows': ['Release/orca_model_runner.exe', 'Release/orca_model_sdk_c.lib',
                          'raw-tbb/tbb12.dll', 'Release/tbb12.dll'],
        'slicing-windows': ['Release/orca_mobile_slicing_json_gate.exe', 'Release/orca_mobile_slicing_sdk_c.lib',
                            'raw-tbb/tbb12.dll', 'Release/tbb12.dll', 'Release/tbbmalloc.dll', 'Release/Clipper2.dll'],
        'model-android': ['approved-tbb-binding.json'],
        'slicing-android': ['libtbb.so'],
    }
    for stage, suffixes in required_layout.items():
        for suffix in suffixes + ['CMakeCache.txt']:
            need(stage + '/' + suffix in inputs, 'incomplete canonical layout: ' + stage + '/' + suffix)
    # Every staged input must belong to the actual producer graph, including
    # compiler input identities and generated/runtime inputs; no arbitrary DLL.
    for stage, item in plan['producers'].items():
        allowed = item['fullIndex'].get('files', item['fullIndex'].get('refs', []))
        source_file = next(x for x in allowed if Path(x['path']).name == 'actual-compiler-inputs.json')
        source_graph = load(pin(source_file))
        def bound(value, target):
            if isinstance(value, dict):
                if all(value.get(k) == v for k, v in target.items()): return True
                return any(bound(x, target) for x in value.values())
            return isinstance(value, list) and any(bound(x, target) for x in value)
        for name, ref in inputs.items():
            if name.startswith(stage + '/'):
                need(ref == item['rawSource']['raw'] or ref in allowed or bound(source_graph, ref),
                     'stage input outside actual source graph: ' + name)
    return tools


def go_closure(plan, go, api):
    expected = plan['toolFiles'] + [x['source'] for x in plan['stageInputs']]
    expected += list(plan.get('ownerQualification', {}).values())
    for ref in plan.get('ownerQualification', {}).values():
        expected.append(api.small_json(api.verify(ref))['report'])
    for item in plan['producers'].values():
        expected += [item['seal']] + item['resourceProof'] + item['sourceProof']
        expected += item['fullIndex'].get('files', item['fullIndex'].get('refs', []))
        expected += list(item.get('originalRecords', {}).values())
    closure = {str(Path(x['path']).resolve()): x for x in go['closure']}
    api.require(len(closure) == len(go['closure']), 'duplicate GO closure member')
    expected_map = {}
    for ref in expected:
        key = str(Path(ref['path']).resolve())
        api.require(key not in expected_map or expected_map[key] == ref, 'conflicting input identities')
        expected_map[key] = ref
    api.require(closure == expected_map, 'GO exact closure differs')
    for ref in expected:
        api.require(closure.get(str(Path(ref['path']).resolve())) == ref, 'GO input closure incomplete')


def owner_protocol(plan, api):
    """Exact local artifact protocol; old Native v5/v6 are never alternatives."""
    import artifact_owner
    import sys
    import shutil
    need = api.require
    need(plan.get('ownerProtocol') == artifact_owner.PROTOCOL, 'artifact owner protocol required')
    need(Path(plan['artifactOwner']).resolve() == Path(artifact_owner.__file__).resolve(), 'not actual artifact owner')
    tools = {str(Path(x['path']).resolve()): x for x in plan['toolFiles']}
    local = tools.get(str(Path(artifact_owner.__file__).resolve()))
    need(local is not None, 'missing local artifact owner pin')
    api.verify(local)
    for name, digest in artifact_owner.SUPPORT.items():
        ref = tools.get(str((Path(plan['ownerDirectory']) / name).resolve()))
        need(ref is not None and ref['sha256'] == digest, 'API-v8 transitive owner identity differs: ' + name)
        api.verify(ref)
        loaded = sys.modules.get(Path(name).stem)
        need(loaded is None or Path(getattr(loaded, '__file__', '')).resolve() == Path(ref['path']).resolve(),
             'shadowed artifact owner transitive module')
    for key, basename in [('python','python.exe'),('node','node.exe'),('readobj','llvm-readobj.exe'),
                          ('readelf','llvm-readelf.exe'),('strip','llvm-strip.exe'),('git','git.exe')]:
        need(Path(plan[key]).name.lower() == basename, 'unexpected artifact image role')
        need(str(Path(plan[key]).resolve()) in tools, 'artifact image missing exact pin')
    need(shutil.which('git') is not None and Path(shutil.which('git')).resolve() == Path(plan['git']).resolve(),
         'canonical packager PATH git differs from pinned artifact image')
    # Qualification is explicitly unknown until different review/QA. Metadata
    # cannot substitute for root's later exact closure-bound execution grant.
    need(plan.get('ownerQualification') is not None, 'artifact owner review/QA unbound')
    for role in ('review', 'qa'):
        ref = plan['ownerQualification'][role]
        receipt = api.small_json(api.verify(ref))
        need(receipt['schema'] == 'sdk-artifact-owner-qualification-v1'
             and receipt['role'] == role and receipt['outcome'] == 'passed'
             and receipt['protocol'] == artifact_owner.PROTOCOL
             and receipt['toolSource'] == plan['toolSource']
             and receipt['owner'] == local, 'artifact owner qualification/version mismatch')
        need(receipt['support'] == {name: tools[str((Path(plan['ownerDirectory'])/name).resolve())]
                                   for name in artifact_owner.SUPPORT}, 'qualification helper mismatch')
        api.verify(receipt['report'])
    return artifact_owner

#!/usr/bin/env python3
"""Read an existing Android Ninja build without modifying it.

Compile this worktree's native units and tests. --link-tests produces a Catch2
Android executable linked to the existing libslic3r/dependencies; no library is
rebuilt or cache copied. Device execution belongs to the coordinating QA agent.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys


def tokens(value):
    return [part.strip('"') for part in shlex.split(value, posix=False)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--link-tests', action='store_true')
    parser.add_argument('--compile-only', action='store_true', help='Compile native/test objects without linking or touching the existing build')
    parser.add_argument('--relink-only', action='store_true', help='Relink existing directed-test objects after dependency library changes')
    options = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    build = options.build_dir.resolve()
    output = options.output_dir.resolve()
    if output == build or build in output.parents:
        parser.error('output-dir must not be inside the read-only existing build')
    output.mkdir(parents=True, exist_ok=True)
    ninja = (build / 'build.ninja').read_text(encoding='utf-8')
    marker = 'build CMakeFiles/orca_mobile_slicing_sdk.dir/'
    block = ninja[ninja.index(marker):].split('\n\n', 1)[0]
    rules = (build / 'CMakeFiles/rules.ninja').read_text(encoding='utf-8')
    rule = re.search(r'rule CXX_COMPILER__orca_mobile_slicing_sdk[^\n]*\n(.*?)(?:\n\n|\Z)', rules, re.S).group(1)
    command = re.search(r'  command = (.*)', rule).group(1)
    command = re.sub(r'\$\{[^}]+\}', '', command)
    prefix = command.split('$DEFINES', 1)[0]
    compiler = tokens(prefix)
    cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
    home = Path(re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', cache, re.M).group(1)).parent
    args = []
    for key in ('DEFINES', 'INCLUDES', 'FLAGS'):
        args.extend(tokens(re.search(r'^  ' + key + r' = (.*)$', block, re.M).group(1)))
    args = [arg for arg in args if not arg.startswith('-DORCA_SLICING_SOURCE_COMMIT')]
    for index, arg in enumerate(args):
        args[index] = arg.replace(home.as_posix() + '/mobile-slicing/../src', source.as_posix() + '/src').replace(
            '-I' + home.as_posix() + '/src', '-I' + source.as_posix() + '/src')
    args += ['-DORCA_SLICING_SOURCE_COMMIT="native-directed-tests"', '-O0', '-g0']
    generated = output / 'include/catch2'
    generated.mkdir(parents=True, exist_ok=True)
    template = (source / 'tests/catch2/src/catch2/catch_user_config.hpp.in').read_text(encoding='utf-8')
    # Default Catch2 configuration, matching an ordinary CMake configuration.
    template = re.sub(r'^#cmakedefine[^\n]*', '// default CMake configuration', template, flags=re.M)
    (generated / 'catch_user_config.hpp').write_text(template, encoding='utf-8')
    # The standalone runner uses the bundled amalgamation. Match its exact
    # headers instead of mixing it with the newer modular Catch2 source tree.
    for wrapper in ('catch_test_macros.hpp', 'matchers/catch_matchers_floating_point.hpp'):
        header = generated / wrapper
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#pragma once\n#include "' + (source / 'tests/catch2/extras/catch_amalgamated.hpp').as_posix() + '"\n', encoding='utf-8')
    test_args = ['-I' + str(output / 'include'), '-I' + str(source / 'tests'),
                 '-I' + str(source / 'tests/catch2/src'), '-DTEST_DATA_DIR="' + (source / 'tests/data').as_posix() + '"']
    units = [source / 'src/orca_mobile_slicing_sdk' / name for name in
             ('project_support.cpp', 'engine.cpp', 'recommendation_support.cpp', 'c_api.cpp')]
    tests = [source / 'tests/slic3rutils/test_orca_slicing_support.cpp',
             source / 'tests/fff_print/test_orca_slicing_preview.cpp']
    results = json.loads((output / "checks.json").read_text(encoding="utf-8")) if options.relink_only and (output / "checks.json").exists() else []

    def run(name, command):
        process = subprocess.run(command, cwd=build, capture_output=True, encoding='utf-8', errors='replace')
        record = {'unit': name, 'exitCode': process.returncode,
                  'stdout': process.stdout, 'stderr': process.stderr, 'command': command}
        results.append(record)
        (output / 'checks.json').write_text(json.dumps(results, indent=2, ensure_ascii=True), encoding='utf-8')
        print(name, 'exit', process.returncode, flush=True)
        if process.stderr:
            print(process.stderr.encode('ascii', 'backslashreplace').decode(), flush=True)
        return process.returncode == 0

    if options.compile_only:
        for unit in units + tests:
            obj = output / (unit.stem + '.o')
            if not run(unit.name + ':object', compiler + args + test_args + ['-c', str(unit), '-o', str(obj)]):
                return 1
        print('Directed objects compiled; no link or device test executed.')
        return 0
    if not options.relink_only:
        for unit in units + tests:
            if not run(unit.name + ':syntax', compiler + args + test_args + ['-fsyntax-only', str(unit)]):
                return 1
    if options.link_tests or options.relink_only:
        objects = []
        for unit in [units[0], units[1], units[2], source / 'src/libslic3r/Format/objparser.cpp'] + tests + [source / 'tests/catch2/extras/catch_amalgamated.cpp']:
            obj = output / (unit.stem + '.o')
            if options.relink_only:
                if not obj.exists() or obj.stat().st_mtime < unit.stat().st_mtime:
                    parser.error('Missing/stale directed-test object: ' + str(obj))
                objects.append(str(obj))
                continue
            if unit.name == 'catch_amalgamated.cpp' and obj.exists() and obj.stat().st_mtime > unit.stat().st_mtime:
                objects.append(str(obj))
                print('Reuse unchanged Catch2 runner object', flush=True)
                continue
            if not run(unit.name + ':object', compiler + args + test_args + ['-c', str(unit), '-o', str(obj)]):
                return 1
            objects.append(str(obj))
        link_marker = 'build liborca_mobile_slicing_sdk.so:'
        link_block = ninja[ninja.index(link_marker):].split('\n\n', 1)[0]
        libs = tokens(re.search(r'^  LINK_LIBRARIES = (.*)$', link_block, re.M).group(1))
        executable = output / 'orca_slicing_support_tests'
        link = compiler + objects + ['-o', str(executable), '-static-libstdc++',
                '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384', '-Wl,--gc-sections'] + libs
        if not run('directed-test-executable:link', link):
            return 1
        print('Android test binary:', executable)
        print('Run with the coordinating device dependency directory on LD_LIBRARY_PATH:')
        print('  ./orca_slicing_support_tests "[OrcaSlicingSupport],[OrcaSlicingPreview]" --order rand')
        print('Compilation/linking is not a test pass; archive the device exit code and Catch2 output.')
    return 0


if __name__ == '__main__':
    sys.exit(main())

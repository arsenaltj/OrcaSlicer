"""Release postprocessing of a NEW Model library, with byte-level runtime proof.

Never use this on an already approved Slicing/TBB bundle. Only --strip-debug is
allowed. The complete unstripped Model is preserved outside the consumer SDK.
"""
from pathlib import Path
import hashlib
import json
import shutil
import struct
import subprocess


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def runtime_identity(path):
    data = Path(path).read_bytes()
    if len(data) < 64:
        raise ValueError('Truncated ELF header')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    if header[0][:6] != b'\x7fELF\x02\x01' or header[2] != 183:
        raise ValueError('Expected AArch64 ELF64 little-endian Model library')
    if (header[8] != 64 or header[9] != 56 or header[11] != 64
            or not header[10] or not header[12] or not 0 < header[13] < header[12]
            or header[5] < 64 or header[5] + header[10] * 56 > len(data)
            or header[6] < 64 or header[6] + header[12] * 64 > len(data)):
        raise ValueError('Unsupported ELF tables')
    # Only these section-table coordinates may change under strip-debug.
    # All other header bytes, including loader entry/PH coordinates, stay bound.
    allowed_header_fields = [(40, 48), (60, 62), (62, 64)]
    normalized_header = bytearray(data[:64])
    for start, end in allowed_header_fields:
        normalized_header[start:end] = bytes(end - start)
    loads = []
    programs = []
    load_contents = []
    for i in range(header[10]):
        entry = struct.unpack_from('<IIQQQQQQ', data, header[5] + i * 56)
        programs.append(list(entry))
        if entry[0] == 1:
            if entry[5] > entry[6] or entry[2] + entry[5] > len(data):
                raise ValueError('Truncated or invalid PT_LOAD range')
            loads.append(list(entry))
            payload = bytearray(data[entry[2]:entry[2] + entry[5]])
            for start, end in allowed_header_fields:
                lo, hi = max(start, entry[2]), min(end, entry[2] + entry[5])
                if lo < hi:
                    payload[lo - entry[2]:hi - entry[2]] = bytes(hi - lo)
            load_contents.append({'offset': entry[2], 'bytes': entry[5],
                                  'sha256': hashlib.sha256(payload).hexdigest()})
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, header[6] + i * 64)
                for i in range(header[12])]
    strings = sections[header[13]]
    if strings[1] != 3 or strings[4] + strings[5] > len(data):
        raise ValueError('Invalid ELF section string table')
    names = data[strings[4]:strings[4] + strings[5]]
    allocated = {}
    debug = []
    for section in sections:
        end = names.find(b'\0', section[0])
        if end < 0:
            raise ValueError('Invalid ELF section name')
        name = names[section[0]:end].decode('ascii')
        if name.startswith('.debug_'):
            debug.append(name)
        if section[2] & 2:  # SHF_ALLOC, including dynsym/dynamic/version/unwind.
            if name in allocated:
                raise ValueError('Duplicate allocated section')
            payload = None if section[1] == 8 else data[section[4]:section[4] + section[5]]
            if payload is not None and len(payload) != section[5]:
                raise ValueError('Truncated allocated section')
            # Bind declared section data to what the loader actually maps.
            # A section-header redirection outside PT_LOAD cannot prove code.
            if not any(
                    load[3] <= section[3] and section[3] + section[5] <= load[3] + load[6]
                    and (section[1] == 8 or (
                        load[2] <= section[4] and section[4] + section[5] <= load[2] + load[5]
                        and section[3] - load[3] == section[4] - load[2]))
                    for load in loads):
                raise ValueError('ALLOC section file/VA mapping is outside PT_LOAD: ' + name)
            allocated[name] = {'type': section[1], 'flags': section[2],
                'address': section[3], 'fileOffset': section[4],
                'size': section[5], 'alignment': section[8],
                'entrySize': section[9],
                'sha256': None if payload is None else hashlib.sha256(payload).hexdigest()}
    if not loads or not {'.text', '.dynsym', '.dynstr', '.dynamic'}.issubset(allocated):
        raise ValueError('Missing ELF runtime identity')
    return {'identityPolicyVersion': 'elf64-load-identity-v2',
            'loaderHeaderSha256': hashlib.sha256(normalized_header).hexdigest(),
            'sectionOnlyHeaderFields': ['e_shoff', 'e_shnum', 'e_shstrndx'],
            'allocatedSections': allocated, 'loadSegments': loads,
            'programHeaders': programs, 'loadedBytes': load_contents}, debug


def require_equal_runtime(original, processed):
    before, before_debug = runtime_identity(original)
    after, after_debug = runtime_identity(processed)
    if before != after:
        raise ValueError('Runtime ELF identity changed; release Model rejected')
    if not before_debug or after_debug:
        raise ValueError('Expected debug sections only in the original Model')
    return before, before_debug


def strip_model(original, tool, output_directory):
    original = Path(original)
    tool = Path(tool)
    output_directory = Path(output_directory)
    if not original.is_absolute() or not tool.is_absolute() or not output_directory.is_absolute():
        raise ValueError('Explicit absolute Model/tool/debug output paths required')
    if (not original.is_file() or original.name != 'liborca_model_sdk_c.so'
            or not tool.is_file() or tool.name not in ('llvm-strip.exe', 'llvm-strip')):
        raise ValueError('Explicit Model library and LLVM strip executable required')
    if output_directory.exists():
        raise ValueError('Debug/release staging already exists; preserve previous evidence')
    before, debug = runtime_identity(original)
    if not debug:
        raise ValueError('Original Model has no debug sections to separate')
    tool_sha = sha(tool)
    tool_version = subprocess.check_output([str(tool), '--version'], text=True).strip()
    if 'LLVM' not in tool_version:
        raise ValueError('LLVM strip identity required')
    output_directory.mkdir(parents=True)
    preserved = output_directory/'liborca_model_sdk_c.unstripped.so'
    processed = output_directory/'liborca_model_sdk_c.so'
    shutil.copyfile(original, preserved)
    raw_sha = sha(original)
    command = [str(tool), '--strip-debug', '-o', str(processed), str(original)]
    subprocess.run(command, check=True, capture_output=True)
    identity, debug = require_equal_runtime(original, processed)
    if sha(original) != raw_sha or sha(preserved) != raw_sha or sha(tool) != tool_sha:
        raise ValueError('Original Model/tool changed during release postprocessing')
    proof = {'schemaVersion': 2, 'operation': 'strip-debug-only-new-model',
        'tool': {'path': str(tool), 'sha256': tool_sha, 'version': tool_version},
        'command': command, 'raw': {'path': str(preserved), 'sha256': raw_sha,
                                  'bytes': preserved.stat().st_size},
        'packaged': {'path': str(processed), 'sha256': sha(processed),
                     'bytes': processed.stat().st_size},
        'removedDebugSections': debug, 'runtimeIdentity': identity,
        'allocatedSectionsByteEqual': True, 'loadSegmentsEqual': True,
        'allocatedFileVaMappingVerified': True, 'loadedBytesVerified': True,
        'minimumLoadAlignment': min(entry[7] for entry in identity['loadSegments']),
        'dynsymVersionNeededUnwindEqual': True, 'originalUnchanged': True}
    (output_directory/'strip-proof.json').write_text(json.dumps(proof, indent=2) + '\n', encoding='utf-8')
    return processed, proof

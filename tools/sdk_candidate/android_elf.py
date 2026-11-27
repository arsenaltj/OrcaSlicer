"""Bounded ELF64 runtime identity, equivalent to strip_android_model v2.

Reads loader and section bytes in chunks; never materializes a large library.
"""
from pathlib import Path
import hashlib
import struct
from adapter import require

HEADER_FIELDS = ((40, 48), (60, 62), (62, 64))


def read_at(stream, offset, size):
    stream.seek(offset)
    data = stream.read(size)
    require(len(data) == size, 'truncated ELF range')
    return data


def range_sha(stream, offset, size, normalize=False, monitor=lambda: None):
    digest = hashlib.sha256()
    stream.seek(offset)
    while size:
        monitor()
        block = bytearray(stream.read(min(size, 1024 * 1024)))
        require(block, 'truncated ELF payload')
        if normalize:
            for start, end in HEADER_FIELDS:
                lo, hi = max(start, offset), min(end, offset + len(block))
                if lo < hi:
                    block[lo-offset:hi-offset] = bytes(hi-lo)
        digest.update(block)
        offset += len(block)
        size -= len(block)
    return digest.hexdigest()


def runtime_identity(path, monitor=lambda: None):
    size = Path(path).stat().st_size
    with Path(path).open('rb') as f:
        header_bytes = read_at(f, 0, 64)
        h = struct.unpack('<16sHHIQQQIHHHHHH', header_bytes)
        require(h[0][:6] == b'\x7fELF\x02\x01' and h[2] == 183, 'AArch64 ELF64 required')
        require(h[8] == 64 and h[9] == 56 and h[11] == 64 and h[10] and h[12]
                and 0 < h[13] < h[12] and h[5] >= 64 and h[6] >= 64
                and h[5]+h[10]*56 <= size and h[6]+h[12]*64 <= size, 'unsupported ELF tables')
        header = bytearray(header_bytes)
        for lo, hi in HEADER_FIELDS:
            header[lo:hi] = bytes(hi-lo)
        programs = [list(struct.unpack('<IIQQQQQQ', read_at(f,h[5]+i*56,56))) for i in range(h[10])]
        loads = [p for p in programs if p[0] == 1]
        require(loads and all(p[5] <= p[6] and p[2]+p[5] <= size for p in loads), 'invalid PT_LOAD')
        require(all(p[7] >= 16384 and p[2] % 16384 == p[3] % 16384 for p in loads), '16KiB PT_LOAD required')
        sections = [struct.unpack('<IIQQQQIIQQ',read_at(f,h[6]+i*64,64)) for i in range(h[12])]
        strings = sections[h[13]]
        require(strings[1] == 3 and strings[5] <= 8*1024*1024 and strings[4]+strings[5] <= size,
                'invalid ELF name table')
        names = read_at(f, strings[4], strings[5])
        allocated, debug = {}, []
        for sec in sections:
            end = names.find(b'\0',sec[0])
            require(0 <= sec[0] < len(names) and end >= sec[0], 'invalid section name')
            name = names[sec[0]:end].decode('ascii')
            if name.startswith('.debug_'):
                debug.append(name)
            if not sec[2] & 2:
                continue
            require(name not in allocated, 'duplicate allocated section')
            require(sec[1] == 8 or sec[4]+sec[5] <= size, 'truncated allocated section')
            require(any(p[3] <= sec[3] and sec[3]+sec[5] <= p[3]+p[6]
                        and (sec[1] == 8 or (p[2] <= sec[4] and sec[4]+sec[5] <= p[2]+p[5]
                        and sec[3]-p[3] == sec[4]-p[2])) for p in loads), 'ALLOC file/VA outside PT_LOAD')
            allocated[name] = dict(type=sec[1],flags=sec[2],address=sec[3],fileOffset=sec[4],
                size=sec[5],alignment=sec[8],entrySize=sec[9],
                sha256=None if sec[1] == 8 else range_sha(f,sec[4],sec[5],monitor=monitor))
        require({'.text','.dynsym','.dynstr','.dynamic'} <= set(allocated), 'missing runtime sections')
        return dict(identityPolicyVersion='elf64-load-identity-v2',
            loaderHeaderSha256=hashlib.sha256(header).hexdigest(),
            sectionOnlyHeaderFields=['e_shoff','e_shnum','e_shstrndx'],allocatedSections=allocated,
            loadSegments=loads,programHeaders=programs,
            loadedBytes=[dict(offset=p[2],bytes=p[5],sha256=range_sha(f,p[2],p[5],True,monitor)) for p in loads]), debug


def source_marker(path, marker, monitor=lambda: None):
    marker = marker.encode('ascii')
    tail = b''
    with Path(path).open('rb') as f:
        while block := f.read(1024*1024):
            monitor()
            if marker in tail+block:
                return True
            tail = block[-(len(marker)-1):]
    return False

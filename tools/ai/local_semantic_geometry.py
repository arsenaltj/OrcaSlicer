"""Bounded ordered mesh interchange; correspondence still requires host proof."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import struct

import numpy as np

MAX_VERTICES = 6_000_000
MAX_FACES = 2_000_000
MAX_BYTES = 184 + 12 * (MAX_VERTICES + MAX_FACES)
HEADER = struct.Struct('<8sQQ64s64s')


def _sha(value: str) -> bool:
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None


def _arrays(vertices, faces):
    v, f = np.asarray(vertices), np.asarray(faces)
    if (v.ndim != 2 or v.shape[1] != 3 or not 0 < len(v) <= MAX_VERTICES or
            v.dtype.kind != 'f' or v.dtype.itemsize != 4 or not np.isfinite(v).all() or
            f.ndim != 2 or f.shape[1] != 3 or not 0 < len(f) <= MAX_FACES or
            f.dtype.kind not in 'iu' or f.min() < 0 or f.max() >= len(v)):
        raise ValueError('Invalid semantic mesh arrays')
    return v, f


def geometry_fingerprint(vertices, faces) -> str:
    v, f = _arrays(vertices, faces)
    digest = hashlib.sha256(b'orca.surface-selection.geometry/v1\0')
    digest.update(struct.pack('<Q', len(f)))
    for start in range(0, len(f), 1024):
        corners = np.array(v[f[start:start + 1024]], dtype='<f4', copy=True)
        corners[corners == 0] = 0  # canonical positive zero
        digest.update(corners.tobytes(order='C'))
    return digest.hexdigest()


def encode(vertices, faces, source_sha256: str) -> bytes:
    if not _sha(source_sha256):
        raise ValueError('Invalid source fingerprint')
    v, f = _arrays(vertices, faces)
    identity = geometry_fingerprint(v, f)
    data = b''.join((HEADER.pack(b'ORCASG01', len(v), len(f), source_sha256.encode('ascii'),
                                identity.encode('ascii')),
                     v.astype('<f4', copy=False).tobytes(), f.astype('<u4', copy=False).tobytes()))
    return data + hashlib.sha256(data).digest()


def decode(data: bytes, expected_source_sha256: str):
    if type(data) is not bytes or not _sha(expected_source_sha256) or not 184 <= len(data) <= MAX_BYTES:
        raise ValueError('Invalid semantic geometry packet')
    magic, nv, nf, source, identity = HEADER.unpack_from(data)
    if (magic != b'ORCASG01' or not 0 < nv <= MAX_VERTICES or not 0 < nf <= MAX_FACES or
            len(data) != 184 + 12 * (nv + nf) or source != expected_source_sha256.encode('ascii') or
            re.fullmatch(b'[0-9a-f]{64}', identity) is None or
            hashlib.sha256(memoryview(data)[:-32]).digest() != data[-32:]):
        raise ValueError('Invalid semantic geometry packet')
    vertices = np.frombuffer(data, dtype='<f4', count=nv * 3, offset=HEADER.size).reshape(nv, 3)
    faces = np.frombuffer(data, dtype='<u4', count=nf * 3, offset=HEADER.size + nv * 12).reshape(nf, 3)
    if geometry_fingerprint(vertices, faces).encode('ascii') != identity:
        raise ValueError('Geometry fingerprint mismatch')
    # Views remain read-only, backed by the verified immutable bytes.
    return vertices, faces, identity.decode('ascii')


def read(path: Path, expected_source_sha256: str):
    with Path(path).open('rb') as stream:
        if not 184 <= os.fstat(stream.fileno()).st_size <= MAX_BYTES:
            raise ValueError('Geometry packet exceeds size limit')
        data = stream.read(MAX_BYTES + 1)
    return decode(data, expected_source_sha256)


def write_new(path: Path, vertices, faces, source_sha256: str):
    """Unique request directory only; publish complete bytes without replacing."""
    path = Path(path)
    data = encode(vertices, faces, source_sha256)
    partial = path.with_name(path.name + '.partial')
    with partial.open('xb') as stream:
        stream.write(data)
    try:
        os.link(partial, path)
    finally:
        partial.unlink()

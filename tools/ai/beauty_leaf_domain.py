"""Canonical midpoint leaves shared by offline boundaries and the workbench."""
from dataclasses import dataclass
import hashlib
import json
import re

import numpy as np


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'),
                                     allow_nan=False).encode()).hexdigest()


@dataclass(frozen=True, order=True)
class LeafKey:
    source_face_id: int
    depth: int = 0
    path: int = 0

    def valid(self, count):
        return all(type(v) is int for v in (self.source_face_id, self.depth, self.path)) and \
            0 <= self.source_face_id < count and 0 <= self.depth <= 4 and 0 <= self.path < 4**self.depth

    def contains(self, other):
        return self.source_face_id == other.source_face_id and self.depth <= other.depth and \
            other.path >> (2*(other.depth-self.depth)) == self.path

    def corners(self):
        if not self.valid(self.source_face_id+1):
            raise ValueError('Invalid midpoint leaf')
        result = np.eye(3)
        for level in range(self.depth):
            a, b, c = result
            ab, bc, ca = (a+b)/2, (b+c)/2, (c+a)/2
            result = np.array(((a, ab, ca), (ab, b, bc), (bc, c, ca),
                               (ab, bc, ca))[(self.path >> (2*(self.depth-level-1))) & 3])
        return result

    def children(self):
        if self.depth >= 4:
            raise ValueError('Maximum leaf depth reached')
        return [LeafKey(self.source_face_id, self.depth+1, self.path*4+i) for i in range(4)]

    def encode(self):
        return [self.source_face_id, self.depth, self.path]


def validate_keys(keys, count):
    if sorted(keys) != keys or len(set(keys)) != len(keys):
        raise ValueError('Leaf keys must be ordered and unique')
    seen = set()
    for key in keys:
        if not key.valid(count):
            raise ValueError('Leaf key out of range')
        for depth in range(key.depth):
            if LeafKey(key.source_face_id, depth, key.path >> (2*(key.depth-depth))) in seen:
                raise ValueError('Leaf ancestor overlap')
        seen.add(key)


def domain(geometry, count, leaves):
    if type(count) is not int or not 0 < count <= 2_000_000 or not re.fullmatch('[0-9a-f]{64}', geometry):
        raise ValueError('Invalid leaf source identity')
    validate_keys(leaves, count)
    coverage = {}
    for key in leaves:
        if not key.depth:
            raise ValueError('Depth-zero roots must be implicit')
        coverage[key.source_face_id] = coverage.get(key.source_face_id, 0) + 4**(-key.depth)
    if any(value != 1 for value in coverage.values()):
        raise ValueError('Incomplete root coverage')
    if len(leaves)-len(coverage) > min(20000, count*2//100):
        raise ValueError('Triangle budget exceeded')
    return {'schema': 'orca.beauty-leaf-domain/v1', 'geometry_id': geometry,
            'face_count': count, 'split_leaves': [key.encode() for key in leaves]}


def locate(leaves, face, bary):
    bary = np.asarray(bary, dtype=float)
    if bary.shape != (3,) or not np.isfinite(bary).all() or bary.min() < -1e-6 or abs(bary.sum()-1) > 1e-5:
        raise ValueError('Invalid canonical barycentric hit')
    candidates = [leaf for leaf in leaves if leaf.source_face_id == face]
    for leaf in sorted(candidates):
        if np.linalg.solve(leaf.corners().T, bary).min() >= -1e-6:
            return leaf
    if candidates:
        raise ValueError('Hit leaves canonical partition')
    return LeafKey(face)

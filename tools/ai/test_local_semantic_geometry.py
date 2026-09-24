import hashlib
from pathlib import Path
import struct
import tempfile
import unittest

import numpy as np
import local_semantic_geometry as transfer


class GeometryTransferTests(unittest.TestCase):
    def setUp(self):
        self.v = np.array([[-0., 0, 0], [1, 0, 0], [0, 1, 0]], dtype=np.float32)
        self.f = np.array([[0, 1, 2]], dtype=np.int32)
        self.source = 'a' * 64

    def test_wire_layout_matches_independent_struct_and_hash(self):
        vertices = struct.pack('<9f', -0., 0., 0., 1., 0., 0., 0., 1., 0.)
        canonical = struct.pack('<9f', 0., 0., 0., 1., 0., 0., 0., 1., 0.)
        identity = hashlib.sha256(b'orca.surface-selection.geometry/v1\0' + struct.pack('<Q', 1) + canonical).hexdigest()
        raw = b'ORCASG01' + struct.pack('<QQ', 3, 1) + self.source.encode() + identity.encode() + vertices + struct.pack('<3I', 0, 1, 2)
        expected = raw + hashlib.sha256(raw).digest()
        self.assertEqual(transfer.encode(self.v, self.f, self.source), expected)
        v, f, actual_id = transfer.decode(expected, self.source)
        self.assertEqual(actual_id, identity)
        self.assertTrue(np.signbit(v[0, 0]))
        self.assertFalse(v.flags.writeable)
        self.assertFalse(f.flags.writeable)
        self.assertEqual(f.tobytes(), self.f.astype('<u4').tobytes())

    def test_mutable_backing_storage_cannot_change_verified_geometry(self):
        raw = transfer.encode(self.v, self.f, self.source)
        for data in (bytearray(raw), memoryview(bytearray(raw)), memoryview(raw)):
            with self.subTest(type=type(data)), self.assertRaises(ValueError):
                transfer.decode(data, self.source)

    def test_corrupt_and_forged_payloads_are_rejected(self):
        good = transfer.encode(self.v, self.f, self.source)
        bad = [good[:-1], good+b'x', b'X'+good[1:]]
        for offset, value in [(8, 255), (16, 0), (24, ord('b')), (88, ord('0')), (188, 3)]:
            data = bytearray(good); data[offset] = value
            data[-32:] = hashlib.sha256(data[:-32]).digest()
            bad.append(bytes(data))
        data = bytearray(good); struct.pack_into('<f', data, 152, float('nan'))
        data[-32:] = hashlib.sha256(data[:-32]).digest(); bad.append(bytes(data))
        for data in bad:
            with self.subTest(data=data[:24]), self.assertRaises(ValueError):
                transfer.decode(data, self.source)

    def test_no_implicit_precision_loss_or_index_conversion(self):
        cases = [(self.v.astype(np.float64), self.f), (self.v, self.f.astype(float)),
                 (self.v, np.array([[-1, 1, 2]])), (self.v, np.array([[0, 1, 3]])),
                 (self.v[:0], self.f), (self.v, self.f[:0])]
        for v, f in cases:
            with self.subTest(dtype=v.dtype), self.assertRaises(ValueError):
                transfer.encode(v, f, self.source)

    def test_packet_checksum_binds_vertex_sharing_even_when_geometry_is_equal(self):
        split = np.concatenate((self.v, self.v[[0]]))
        split_faces = np.array([[3, 1, 2]], dtype=np.uint32)
        self.assertEqual(transfer.geometry_fingerprint(self.v, self.f), transfer.geometry_fingerprint(split, split_faces))
        self.assertNotEqual(transfer.encode(self.v, self.f, self.source), transfer.encode(split, split_faces, self.source))

    def test_publish_never_replaces_existing_packet(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'中文 mesh.bin'
            transfer.write_new(path, self.v, self.f, self.source)
            original = path.read_bytes()
            self.assertEqual(transfer.read(path, self.source)[2], transfer.geometry_fingerprint(self.v, self.f))
            with self.assertRaises(FileExistsError):
                transfer.write_new(path, self.v, self.f, self.source)
            self.assertEqual(path.read_bytes(), original)
            self.assertFalse(path.with_name(path.name+'.partial').exists())


if __name__ == '__main__':
    unittest.main()

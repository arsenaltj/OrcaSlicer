"""Real new-Model strip-debug proof and runtime-corruption rejection."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('strip_model', SOURCE/'tools/strip_android_model.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class StripTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='new-model-strip-', dir=ARGS.workspace)
        cls.root = Path(cls.temp.name)
        cls.original = Path(ARGS.model)
        cls.original_sha = guard.sha(cls.original)
        cls.processed, cls.proof = guard.strip_model(cls.original, ARGS.tool, cls.root/'valid')

    @classmethod
    def tearDownClass(cls):
        assert guard.sha(cls.original) == cls.original_sha
        cls.temp.cleanup()

    def changed(self, name, mutate):
        data = bytearray(self.processed.read_bytes())
        mutate(data)
        target = self.root/(name+'.so')
        target.write_bytes(data)
        return target

    def section_record(self, data, desired):
        header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
        sections = [struct.unpack_from('<IIQQQQIIQQ', data, header[6]+64*i) for i in range(header[12])]
        names = sections[header[13]]
        strings = data[names[4]:names[4]+names[5]]
        for index, section in enumerate(sections):
            name = strings[section[0]:strings.find(b'\0', section[0])].decode()
            if name == desired:
                return header, index, section
        self.fail('Missing runtime section '+desired)

    def section_offset(self, data, desired):
        return self.section_record(data, desired)[2][4]

    def test_real_strip_preserves_runtime_and_original(self):
        identity, removed = guard.require_equal_runtime(self.original, self.processed)
        self.assertEqual(identity, self.proof['runtimeIdentity'])
        self.assertGreater(len(removed), 0)
        self.assertLess(self.processed.stat().st_size, self.original.stat().st_size)
        self.assertEqual(guard.sha(self.original), self.original_sha)
        self.assertEqual(self.proof['command'][1], '--strip-debug')
        self.assertEqual(guard.sha(self.proof['raw']['path']), self.original_sha)
        self.assertEqual(json.loads((self.processed.parent/'strip-proof.json').read_text()), self.proof)
        self.assertEqual(identity['identityPolicyVersion'], 'elf64-load-identity-v2')
        self.assertTrue(self.proof['allocatedFileVaMappingVerified'])
        self.assertTrue(self.proof['loadedBytesVerified'])

    def test_section_redirection_cannot_mask_changed_loaded_code(self):
        def mutate(data):
            header, index, text = self.section_record(data, '.text')
            copy_offset = len(data)
            data.extend(data[text[4]:text[4]+text[5]])
            struct.pack_into('<Q', data, header[6]+index*64+24, copy_offset)
            data[text[4]] ^= 1
        altered = self.changed('redirected-text-with-loaded-code-change', mutate)
        with self.assertRaisesRegex(ValueError, 'ALLOC section file/VA mapping is outside PT_LOAD'):
            guard.require_equal_runtime(self.original, altered)

    def test_alloc_offset_must_match_load_virtual_address(self):
        def mutate(data):
            header, index, text = self.section_record(data, '.text')
            struct.pack_into('<Q', data, header[6]+index*64+24, text[4]+4)
        altered = self.changed('incorrect-file-va-map', mutate)
        with self.assertRaisesRegex(ValueError, 'ALLOC section file/VA mapping is outside PT_LOAD'):
            guard.require_equal_runtime(self.original, altered)

    def test_loaded_padding_outside_sections_is_bound(self):
        def mutate(data):
            header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
            occupied = [(0, 64), (header[5], header[5]+header[10]*56)]
            for index in range(header[12]):
                section = struct.unpack_from('<IIQQQQIIQQ', data, header[6]+index*64)
                if section[2] & 2 and section[1] != 8 and section[5]:
                    occupied.append((section[4], section[4]+section[5]))
            for index in range(header[10]):
                load = struct.unpack_from('<IIQQQQQQ', data, header[5]+index*56)
                if load[0] != 1:
                    continue
                cursor, end = load[2], load[2]+load[5]
                for lo, hi in sorted(occupied):
                    if hi <= cursor:
                        continue
                    if cursor < min(lo, end):
                        data[cursor] ^= 1
                        return
                    cursor = max(cursor, hi)
                    if cursor >= end:
                        break
                if cursor < end:
                    data[cursor] ^= 1
                    return
            self.fail('Real Model has no loaded padding to exercise')
        altered = self.changed('changed-loaded-padding', mutate)
        with self.assertRaisesRegex(ValueError, 'Runtime ELF identity changed'):
            guard.require_equal_runtime(self.original, altered)

    def test_non_section_header_fields_are_not_ignored(self):
        for offset in [7, 24]:  # OS ABI and entry point, never strip exceptions.
            with self.subTest(offset=offset):
                def mutate(data):
                    data[offset] ^= 1
                altered = self.changed('header-'+str(offset), mutate)
                with self.assertRaisesRegex(ValueError, 'Runtime ELF identity changed'):
                    guard.require_equal_runtime(self.original, altered)

    def test_allowed_section_coordinates_still_require_valid_tables(self):
        for name, offset, fmt, value in [('shoff', 40, '<Q', self.processed.stat().st_size+8),
                                          ('shnum', 60, '<H', 0), ('shstrndx', 62, '<H', 65535)]:
            with self.subTest(field=name):
                def mutate(data):
                    struct.pack_into(fmt, data, offset, value)
                altered = self.changed('invalid-'+name, mutate)
                with self.assertRaisesRegex(ValueError, 'Unsupported ELF tables'):
                    guard.require_equal_runtime(self.original, altered)

    def test_changed_code_dynsym_and_unwind_rejected(self):
        for section in ['.text', '.dynsym', '.eh_frame', '.dynamic']:
            with self.subTest(section=section):
                def mutate(data):
                    offset = self.section_offset(data, section)
                    data[offset] ^= 1
                altered = self.changed('altered-'+section, mutate)
                with self.assertRaisesRegex(ValueError, 'Runtime ELF identity changed'):
                    guard.require_equal_runtime(self.original, altered)

    def test_changed_load_flags_or_alignment_rejected(self):
        for field_offset in [4, 48]:
            with self.subTest(field=field_offset):
                def mutate(data):
                    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
                    for index in range(header[10]):
                        offset = header[5]+index*56
                        if struct.unpack_from('<I', data, offset)[0] == 1:
                            data[offset+field_offset] ^= 1
                            return
                    self.fail('Missing load segment')
                altered = self.changed('load-'+str(field_offset), mutate)
                with self.assertRaisesRegex(ValueError, 'Runtime ELF identity changed'):
                    guard.require_equal_runtime(self.original, altered)

    def test_preexisting_output_is_preserved(self):
        original = self.processed.read_bytes()
        with self.assertRaisesRegex(ValueError, 'already exists'):
            guard.strip_model(self.original, ARGS.tool, self.processed.parent)
        self.assertEqual(self.processed.read_bytes(), original)

    def test_producer_rejects_full_debug_artifact_inside_sdk(self):
        target = self.root/'consumer-sdk'
        result = subprocess.run([sys.executable, str(SOURCE/'tools/package_sdk.py'),
            '--windows-build', str(self.root/'host'), '--android-build', str(self.root/'android'),
            '--output', str(target), '--android-strip-tool', str(ARGS.tool),
            '--android-debug-output', str(target/'debug')], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('outside the consumer SDK', result.stderr)
        self.assertFalse(target.exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--model', required=True)
    parser.add_argument('--tool', required=True)
    parser.add_argument('--workspace', required=True)
    ARGS, rest = parser.parse_known_args()
    Path(ARGS.workspace).mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[sys.argv[0]]+rest)

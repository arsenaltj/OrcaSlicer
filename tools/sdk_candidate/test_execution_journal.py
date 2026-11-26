"""Tiny persistence fixtures only; no native or process calls."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from execution_journal import ExecutionJournal, reconstruct, canonical


def state():
    return dict(state='started', commands=[], entry=[dict(freeKiB=99)],
                sourceAdmissionSamples=[dict(utc='initial', processes=[None])], ioSamples=[])


class JournalScale(unittest.TestCase):
    def test_full_execute_append_error_prevents_first_command_and_publication(self):
        from test_package_android import AndroidPhase
        original=ExecutionJournal.append
        def broken(journal,kind,value):
            if kind=='io':
                journal.failed=True
                raise OSError('first journal append failure')
            return original(journal,kind,value)
        with patch.object(ExecutionJournal,'append',broken):
            result=AndroidPhase().exercise(memory=True)
        self.assertEqual(result['error'],'first journal append failure')
        self.assertFalse(result['owners']);self.assertFalse(result['published'])
        self.assertIn('first journal append failure',result['state']['error'])
        self.assertIn('permanently failed',result['state']['journalPersistenceError'])

    def test_failure_finalization_error_preserves_real_execute_first_exception(self):
        from test_package_android import AndroidPhase
        with patch.object(ExecutionJournal,'finish',side_effect=OSError('secondary journal failure')):
            result=AndroidPhase().exercise('late-output')
        self.assertFalse(result['published'])
        self.assertNotEqual(result['error'],'secondary journal failure')
        self.assertIn('FileExistsError',result['state']['error'])
        self.assertIn('secondary journal failure',result['state']['journalPersistenceError'])

    def test_short_write_is_sticky_before_any_receipt(self):
        with tempfile.TemporaryDirectory() as d:
            j=ExecutionJournal(d);real=j.stream
            class Short:
                def write(self,value):return len(value)-1
                def close(self):real.close()
            j.stream=Short()
            try:
                with self.assertRaisesRegex(OSError,'short'):j.save(state())
                with self.assertRaisesRegex(OSError,'permanently'):j.save(state())
                self.assertFalse(j.final.exists())
            finally:j.close()

    def test_complete_lossless_mutable_command_and_final_schema(self):
        with tempfile.TemporaryDirectory() as d:
            s=state();j=ExecutionJournal(d)
            try:
                j.save(s)
                for n in range(3):
                    s['commands'].append(dict(label=str(n),rawSamples=[]))
                    for i in range(3):
                        s['commands'][-1]['rawSamples'].append(dict(processes=[None,dict(pid=i)],utc=str(i)))
                        j.save(s)
                    s['commands'][-1]['state']='returned'
                s['ioSamples'].append(dict(utc='io',nullable=None));s['state']='android_candidate_phone_pending'
                j.finish(s)
                self.assertEqual(canonical(reconstruct(j.path)),canonical(s))
                self.assertEqual(json.loads(j.final.read_text()),s)
            finally:j.close()

    def test_source_io_growth_linear_and_no_unchanged_history_rewrite(self):
        with tempfile.TemporaryDirectory() as d:
            s=state();j=ExecutionJournal(d)
            try:
                sizes=[]
                for i in range(128):
                    s['ioSamples'].append(dict(utc=i,processes=[dict(nullable=None,text='x'*64)]));j.save(s)
                    if i in (63,127):sizes.append(j.path.stat().st_size)
                self.assertLess(sizes[1],sizes[0]*2.1)
                before=j.path.read_bytes();j.save(s);self.assertEqual(before,j.path.read_bytes())
                self.assertEqual(len(reconstruct(j.path)['ioSamples']),128)
                j.finish(s)
            finally:j.close()

    def test_completed_commands_are_not_reserialized(self):
        with tempfile.TemporaryDirectory() as d:
            s=state();j=ExecutionJournal(d)
            try:
                s['commands']=[dict(rawSamples=[dict(raw='old')])];j.command_finished(s)
                class Poison(dict):
                    def items(self):raise AssertionError('closed history visited')
                s['commands'][0]=Poison(s['commands'][0])
                s['ioSamples'].append(dict(utc=2));j.save(s)
                self.assertEqual(reconstruct(j.path)['commands'][0],dict(rawSamples=[dict(raw='old')]))
            finally:j.close()

    def test_partial_and_corrupt_append_cannot_reconstruct_success(self):
        with tempfile.TemporaryDirectory() as d:
            j=ExecutionJournal(d);j.save(state());j.close();raw=j.path.read_bytes()
            for bad in (raw[:-1],raw.replace(b'initial',b'injtial',1),b'',raw+raw):
                j.path.write_bytes(bad)
                with self.assertRaises((ValueError,KeyError)):reconstruct(j.path)
            row=json.loads(raw.splitlines()[0]);row['seq']=False
            import hashlib
            row.pop('sha256');row['sha256']=hashlib.sha256(canonical(row).encode()).hexdigest()
            j.path.write_text(canonical(row)+'\n')
            with self.assertRaisesRegex(ValueError,'envelope'):reconstruct(j.path)

    def test_append_fsync_failure_is_sticky_and_no_success_receipt(self):
        with tempfile.TemporaryDirectory() as d:
            j=ExecutionJournal(d)
            try:
                with patch('execution_journal.os.fsync',side_effect=OSError('disk')):
                    with self.assertRaises(OSError):j.save(state())
                with self.assertRaises(OSError):j.finish(state())
                self.assertFalse(j.final.exists());self.assertTrue(j.failed)
            finally:j.close()

    def test_final_equivalence_rejects_old_sample_mutation_or_typed_change(self):
        for replacement in (1,1.0,False):
            with tempfile.TemporaryDirectory() as d:
                s=state();s['entry']=[True];j=ExecutionJournal(d)
                try:
                    j.save(s);s['entry']=[replacement]
                    with self.assertRaises(ValueError):j.finish(s)
                    self.assertFalse(j.final.exists())
                finally:j.close()

    def test_final_write_error_fails_closed(self):
        with tempfile.TemporaryDirectory() as d:
            j=ExecutionJournal(d)
            try:
                with patch('execution_journal.os.replace',side_effect=OSError('replace')):
                    with self.assertRaises(OSError):j.finish(state())
                self.assertFalse(j.final.exists())
            finally:j.close()

    def test_fields_deletion_and_failure_first_error_preserved(self):
        with tempfile.TemporaryDirectory() as d:
            s=state();s['transient']=True;j=ExecutionJournal(d)
            try:
                j.save(s);del s['transient'];s.update(state='failed_partial_no_retry',error='original cancel')
                j.finish(s);self.assertEqual(reconstruct(j.path),s)
                self.assertEqual(json.loads(j.final.read_text())['error'],'original cancel')
            finally:j.close()

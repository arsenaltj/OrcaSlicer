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
    def test_partial_final_pending_and_final_file_close_failure_preserve_failed_receipt(self):
        import execution_journal as module
        from test_package_android import AndroidPhase
        write=ExecutionJournal.write_final
        def partial(j,s):
            if s['state']=='android_candidate_phone_pending':
                j.final.with_suffix('.pending.json').write_text('retained partial')
                raise OSError('first final body write')
            self.assertEqual(j.final.with_suffix('.pending.json').read_text(),'retained partial')
            return write(j,s)
        with patch.object(ExecutionJournal,'write_final',partial):r=AndroidPhase().exercise(memory=True)
        self.assertFalse(r['published']);self.assertEqual(r['error'],'first final body write')
        self.assertEqual(r['state']['state'],'failed_partial_no_retry')
        terminal=module.close_stream
        def broken(stream,status,close,first=None):
            if status.get('path','').endswith('execution.pending.json'):
                def report_error():close();raise OSError('final JSON close error')
                return terminal(stream,status,report_error,first)
            return terminal(stream,status,close,first)
        with patch.object(module,'close_stream',broken):r=AndroidPhase().exercise(memory=True)
        self.assertFalse(r['published']);self.assertEqual(r['error'],'final JSON close error')
        self.assertTrue(r['state']['finalReceiptTerminal']['observedClosed'])
        self.assertIn('final JSON close error',r['state']['finalReceiptTerminal']['closeError'])
        self.assertTrue(r['state']['journalRecovery']['rawArraysAndCommandsEquivalent'])

    def test_close_error_after_real_close_prevents_publication_and_checks_all_raw(self):
        from test_package_android import AndroidPhase
        close=ExecutionJournal.close;calls=[]
        def fail(j):
            close(j);calls.append(True);raise OSError('terminal close after actual close')
        with patch.object(ExecutionJournal,'close',fail):r=AndroidPhase().exercise(memory=True)
        self.assertEqual(calls,[True]);self.assertFalse(r['published'])
        self.assertEqual(r['error'],'terminal close after actual close')
        self.assertEqual(r['state']['state'],'failed_partial_no_retry')
        self.assertTrue(r['state']['journalTerminal']['observedClosed'])
        self.assertTrue(r['state']['journalRecovery']['rawArraysAndCommandsEquivalent'])
        self.assertEqual(r['state']['journalRecovery']['lastCompleteStage'],'android_candidate_phone_pending')
        self.assertFalse(r['state']['journalRecovery']['failureFieldsAppended'])

    def test_earlier_business_failure_remains_first_when_terminal_close_fails(self):
        from test_package_android import AndroidPhase
        close=ExecutionJournal.close;calls=[]
        def fail(j):
            close(j);calls.append(True);raise OSError('secondary terminal close')
        with patch.object(ExecutionJournal,'close',fail):r=AndroidPhase().exercise('late-output')
        self.assertFalse(r['published']);self.assertEqual(calls,[True])
        self.assertIn('FileExistsError',r['state']['error'])
        self.assertIn('secondary terminal close',r['state']['journalTerminal']['closeError'])
        self.assertTrue(r['state']['journalRecovery']['rawArraysAndCommandsEquivalent'])

    def test_indeterminate_close_not_retried_and_flush_failure_still_closes_once(self):
        with tempfile.TemporaryDirectory() as d:
            j=ExecutionJournal(d);j.save(state())
            try:
                with patch.object(j,'close',return_value=None):
                    with self.assertRaisesRegex(OSError,'not observed complete'):j.finish(state())
                self.assertFalse(j.final.exists());self.assertFalse(j.terminal['observedClosed'])
            finally:j.stream.close()
        with tempfile.TemporaryDirectory() as d:
            j=ExecutionJournal(d);j.save(state());calls=[]
            def unknown():calls.append(True);raise OSError('unknown close status')
            try:
                with patch.object(j,'close',unknown):
                    with self.assertRaises(OSError):j.finish(state())
                    with self.assertRaises(OSError):j.terminate()
                self.assertEqual(calls,[True]);self.assertFalse(j.terminal['observedClosed'])
                self.assertTrue(j.terminal['closeAttempted']);self.assertFalse(j.final.exists())
            finally:j.stream.close() # fixture releases its known real stream, never a production retry
        with tempfile.TemporaryDirectory() as d:
            j=ExecutionJournal(d);j.save(state());close=j.close;calls=[]
            def counted():calls.append(True);close()
            with patch.object(j,'close',counted),patch('execution_journal.os.fsync',side_effect=OSError('terminal fsync')):
                with self.assertRaises(OSError):j.terminate()
            self.assertEqual(calls,[True]);self.assertTrue(j.terminal['observedClosed'])
            self.assertIn('terminal fsync',j.terminal['error'])

    def test_result_atomic_replace_failure_after_close_is_failed_not_published(self):
        import adapter
        from test_package_android import AndroidPhase
        replace=adapter.os.replace
        def fail(source,destination):
            if Path(destination).name=='candidate-result.json':raise OSError('result atomic replace failure')
            return replace(source,destination)
        with patch.object(adapter.os,'replace',fail):r=AndroidPhase().exercise(memory=True)
        self.assertEqual(r['error'],'result atomic replace failure');self.assertFalse(r['published'])
        self.assertTrue(r['state']['journalTerminal']['observedClosed'])
        self.assertTrue(r['state']['journalRecovery']['rawArraysAndCommandsEquivalent'])

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

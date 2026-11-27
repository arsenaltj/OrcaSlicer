"""Small synthetic regressions; no actual producer/manifest/raw reads."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import adapter as a
import contracts
import test_fix1


class Fix2(unittest.TestCase):
    def test_review_substituted_failed_same_name_all_stages(self):
        for stage in a.STAGES:
            with self.subTest(stage=stage), tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp)
                plan=test_fix1.Fix1().fixture(root)
                item=plan['producers'][stage]
                original_resource=a.small_json(item['resourceProof'][0]['path'])['summary']
                resource_bytes=Path(original_resource['path']).read_bytes()
                wrong=root/'unrelated'/stage/'execution.json'
                wrong.parent.mkdir(parents=True)
                wrong.write_text(json.dumps(dict(source=a.SOURCE,state='failed',exitCode=72,
                                               runtimeViolations=['memory'],resourceReturned=False)))
                wrongpin=a.identity(wrong)
                item['fullIndex']['files'].append(wrongpin)
                sealpath=Path(item['seal']['path'])
                sealpath.write_text(json.dumps(item['fullIndex']))
                item['seal']=a.identity(sealpath)
                for role in ('resourceProof','sourceProof'):
                    path=Path(item[role][0]['path'])
                    summary=json.loads(path.read_bytes())
                    summary.update(execution=wrongpin,seal=item['seal'])
                    path.write_text(json.dumps(summary))
                    item[role]=[a.identity(path)]
                with self.assertRaisesRegex(ValueError,'exact original execution'):
                    contracts.check(plan,a)
                self.assertEqual(Path(original_resource['path']).read_bytes(),resource_bytes)

    def test_original_command_only_resource_requires_exact_original_index(self):
        # Legacy resources without a direct SHA field use their immutable
        # original same-directory seal member; no typed boolean grants trust.
        with tempfile.TemporaryDirectory() as tmp, patch.object(contracts.subprocess,'check_output',
             side_effect=lambda argv,**kw:'a'*40+'\n' if argv[-1]=='HEAD' else ''):
            plan=test_fix1.Fix1().fixture(Path(tmp))
            item=plan['producers']['model-android']
            summary_path=Path(a.small_json(item['resourceProof'][0]['path'])['summary']['path'])
            summary=a.small_json(summary_path)
            summary.pop('originalRecordSha256');summary.pop('originalRecordBytes')
            summary_path.write_text(json.dumps(summary))
            summary_pin=a.identity(summary_path)
            item['fullIndex']['files']=[summary_pin if x['path']==str(summary_path) else x for x in item['fullIndex']['files']]
            seal=Path(item['seal']['path']);seal.write_text(json.dumps(item['fullIndex']));item['seal']=a.identity(seal)
            for role in ('resourceProof','sourceProof'):
                path=Path(item[role][0]['path']);data=a.small_json(path);data['seal']=item['seal']
                if role=='resourceProof': data['summary']=summary_pin
                path.write_text(json.dumps(data));item[role]=[a.identity(path)]
            contracts.check(plan,a)
            item=plan['producers']['model-android']
            item['fullIndex']['files']=[x for x in item['fullIndex']['files'] if Path(x['path']).name!='execution.json']
            seal=Path(item['seal']['path']);seal.write_text(json.dumps(item['fullIndex']))
            item['seal']=a.identity(seal)
            for role in ('resourceProof','sourceProof'):
                path=Path(item[role][0]['path']);data=a.small_json(path);data['seal']=item['seal']
                path.write_text(json.dumps(data));item[role]=[a.identity(path)]
            with self.assertRaises(ValueError): contracts.check(plan,a)

    def test_pinned_synthetic_dependency_manifest_above_small_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'dependency-input-manifest-v1.json'
            data=[dict(path=str(Path(tmp)/'dep'),files=[dict(path='original-source',sha256='a'*64)],
                       syntheticPadding='x'*(4*1024*1024+1))]
            path.write_text(json.dumps(data))
            ref=a.identity(path)
            self.assertGreater(ref['bytes'],4*1024*1024)
            with self.assertRaisesRegex(ValueError,'not a small metadata'):
                a.small_json(path)
            self.assertEqual(a.dependency_json(ref),data)
            wrong=dict(ref,sha256='0'*64)
            with self.assertRaises(ValueError): a.dependency_json(wrong)
            path.write_text('[]')
            with self.assertRaises(ValueError): a.dependency_json(ref)

    def test_dependency_size_missing_and_execution_fail_before_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'missing.json'
            for size in (0,16*1024*1024+1):
                with self.assertRaisesRegex(ValueError,'size bound'):
                    a.dependency_json(dict(path=str(path),bytes=size,sha256='0'*64))
            with self.assertRaises((ValueError,FileNotFoundError)):
                a.dependency_json(dict(path=str(path),bytes=2,sha256='0'*64))
            execution=Path(tmp)/'execution.json';execution.write_text('[]')
            with self.assertRaisesRegex(ValueError,'not a canonical'):
                a.dependency_json(a.identity(execution))


if __name__=='__main__': unittest.main()

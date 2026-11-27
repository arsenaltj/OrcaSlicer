"""Historical 4bf tests keep their exact b5f metadata and source profile.
No production package execution or source override is performed here.
"""
import unittest
from unittest.mock import patch
import json
import adapter as a
import package_android as p
import normal_android_producer as n
import test_package_android as old

OLD_SOURCE='b5f078f52a8db9bb8eb3b347b155679c852d5c32'
OLD_TREE='cd7a99640b2c396917176e1e7e9ee1e356b6915a'
OLD_ORIGINS={
 'model-android':dict(seal='1e6a88d930f12e208a68347fda510ada180cb46069553966e01cd34fa328f3e7',
 rawSha='90b81c248670999ef071432f489138d65dad329295f9966a3d86eb00a5c76dd8',rawBytes=81377656),
 'slicing-android':dict(seal='32cca4d5383acc6a6ad11e9b68d767a917493d896da2300ce45535af4eb76b81',
 rawSha='eafe048e5196ae43fd71f2d556168679219c38d28bfaee3eaf6ec37696cda847',rawBytes=704366320)}

class HistoricalProfile(unittest.TestCase):
    def test_original29_under_explicit_4bf_profile(self):
        current=(a.SOURCE,a.TREE,p.ORIGINS)
        with patch.object(a,'SOURCE',OLD_SOURCE),patch.object(a,'TREE',OLD_TREE),patch.object(p,'ORIGINS',OLD_ORIGINS):
            suite=unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(cls) for cls in
                [old.AndroidPackage,old.AndroidPinTracks,old.AndroidNamespace])
            result=unittest.TestResult();suite.run(result)
            self.assertEqual(result.testsRun,29)
            self.assertFalse(result.errors or result.failures, result.errors+result.failures)
        self.assertEqual((a.SOURCE,a.TREE,p.ORIGINS),current)

    def test_current_default_rejects_old_actual_metadata(self):
        self.assertEqual(a.SOURCE,n.SOURCE);self.assertEqual(a.TREE,n.TREE)
        plan=json.loads((old.AndroidConcreteContract.EVIDENCE/
            'android-concrete-binding-preparation-v2/observe-plan-DISABLED.json').read_text(encoding='utf-8'))
        for stage,item in plan['producers'].items():
            self.assertEqual(item['rawSource']['source'],OLD_SOURCE)
            with self.subTest(stage=stage),self.assertRaisesRegex(ValueError,'wrong SDK source'):
                a.terminal_ok(stage,item['terminal'],item['rawSource'])

if __name__=='__main__':unittest.main()

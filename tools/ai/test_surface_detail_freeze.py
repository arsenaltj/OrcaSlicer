import copy
import unittest

from surface_detail_freeze import capture, validate


class DetailFreezeTests(unittest.TestCase):
    def fixture(self):
        polygon = [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]]
        document = dict(geometry_id='a' * 64, source_sha256='b' * 64, face_count=10,
                        faces=[dict(source_face_id=1, cells=[dict(id='c' * 64, label='le', parent_label='le',
                                subject_id='subject', polygon=polygon, holes=[])])])
        lock = dict(label='le', parent_label='le', subject_id='subject', status='PROTECTED_SHAPE_UNCERTAIN',
                    view_support=2, reasons=['FIT_RISK'], locked_cells=['c' * 64], nested_cells=[], periocular_cells=[])
        locks = dict(locks=[lock])
        plans = {3: dict(palette=[dict(uid='white', rgb=[1., 1., 1.])],
                    cells=[dict(id='c' * 64, slot_uid='white', color_source='VERIFIED_SOURCE')])}
        return document, locks, plans

    def test_unrelated_partition_change_does_not_invalidate_freeze(self):
        document, locks, plans = self.fixture()
        freeze = capture(document, locks, plans, ('le',))
        document['partition_sha256'] = 'd' * 64
        document['boundary_policy_sha256'] = 'e' * 64
        self.assertTrue(validate(document, locks, plans, freeze))

    def test_boundary_nested_risk_color_and_identity_drift_fail_closed(self):
        original = self.fixture()
        freeze = capture(*original, ('le',))
        for field in ('polygon', 'nested', 'status', 'slot', 'rgb', 'source', 'stable_id', 'hole'):
            with self.subTest(field=field):
                document, locks, plans = copy.deepcopy(original)
                if field == 'polygon': document['faces'][0]['cells'][0]['polygon'][0] = [.9, .1, 0.]
                elif field == 'nested': locks['locks'][0]['nested_cells'] = ['c' * 64]
                elif field == 'status': locks['locks'][0]['status'] = 'VALID_SHAPE'
                elif field == 'slot': plans[3]['cells'][0]['slot_uid'] = None
                elif field == 'rgb': plans[3]['palette'][0]['rgb'][0] = .9
                elif field == 'source': document['source_sha256'] = 'f' * 64
                elif field == 'stable_id': document['faces'][0]['cells'][0]['id'] = 'f' * 64
                elif field == 'hole': document['faces'][0]['cells'][0]['holes'] = [[[.7,.2,.1],[.6,.3,.1],[.6,.2,.2]]]
                with self.assertRaises((ValueError, KeyError)):
                    validate(document, locks, plans, freeze)

    def test_malformed_fingerprint_is_rejected(self):
        data = self.fixture()
        freeze = capture(*data, ('le',))
        freeze['details'][0]['reasons'] = []
        with self.assertRaisesRegex(ValueError, 'fingerprint'):
            validate(*data, freeze)


if __name__ == '__main__':
    unittest.main()

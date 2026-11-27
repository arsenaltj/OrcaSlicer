"""3MF angle regression fixtures and public-CABI assertions.
Preparation/unit checks are pure Python; exercise() requires a real SDK and is
never called by the pure tests. Source archives are copied, never rewritten.
"""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile


def project(source, destination, angles, second=None, unknown=False):
    source, destination = Path(source), Path(destination)
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    with zipfile.ZipFile(source) as src, zipfile.ZipFile(destination, 'x') as dst:
        for item in src.infolist():
            data = src.read(item.filename)
            if item.filename.lower() == 'metadata/model_settings.config':
                doc = ET.fromstring(data)
                plates = doc.findall('plate')
                assert len(plates) == 1
                plate = plates[0]
                for entry in list(plate):
                    if entry.tag == 'metadata' and entry.get('key') == 'skirt_start_angle':
                        plate.remove(entry)
                for value in angles:
                    ET.SubElement(plate, 'metadata', key='skirt_start_angle', value=value)
                if unknown:
                    ET.SubElement(plate, 'metadata', key='future_skirt_semantics', value='1')
                if second is not None:
                    other = copy.deepcopy(plate)
                    for entry in list(other):
                        if entry.tag == 'metadata' and entry.get('key') == 'plater_id':
                            entry.set('value', '2')
                        if entry.tag == 'metadata' and entry.get('key') == 'skirt_start_angle':
                            other.remove(entry)
                    ET.SubElement(other, 'metadata', key='skirt_start_angle', value=second)
                    # Two original instances, one per plate, as in the public suite.
                    members = plate.findall('model_instance')
                    others = other.findall('model_instance')
                    assert len(members) == len(others) == 2
                    plate.remove(members[1]); other.remove(others[0])
                    doc.append(other)
                data = ET.tostring(doc, encoding='utf-8')
            dst.writestr(item, data)
    assert hashlib.sha256(source.read_bytes()).hexdigest() == before
    return destination


def exercise(run, success, basis, root):
    # Public harness saves each real result, source SHA and emitted events.
    for i, value in enumerate(('-180', '-135', '0', '180', '45.5', '1e1')):
        path = project(basis, root / ('skirt-valid-%d.3mf' % i), [value])
        out = success('skirt-import-%d' % i, path)['project']
        assert not out['blockingFeatures'], out['blockingFeatures']
        declared = out['threeMfInspection']['plates'][0]['validatedNativeSkirtStartAngle']
        actual = json.loads(Path(out['sourceProfilePath']).read_text())['config']['skirt_start_angle']
        assert float(actual) == declared == float(value)
    path = project(basis, root / 'skirt-source-override.3mf', ['-135'])
    out = success('skirt-explicit-override', path, config={'skirt_start_angle': '45'})['project']
    assert float(json.loads(Path(out['sourceProfilePath']).read_text())['config']['skirt_start_angle']) == -135
    assert float(out['config']['skirt_start_angle']) == 45
    out = success('skirt-slice', path, 'slice', executeEffectiveConfig=True)
    assert float(out['effectiveConfig']['skirt_start_angle']) == -135
    exported = success('skirt-export', path, 'exportProject', confirmDerivedProject=True)
    again = success('skirt-reimport', Path(exported['artifacts'][0]['path']))['project']
    assert not again['blockingFeatures'] and float(again['config']['skirt_start_angle']) == -135
    equal = project(basis, root / 'skirt-equal.3mf', ['-135', '-135.0'])
    assert not success('skirt-equal', equal)['project']['blockingFeatures']
    bad = ['+45.5', '', 'nan', 'inf', '-inf', '181', '-180.1', '1junk', '45%', '1,2', ' 1', '1 ', '1' * 1025]
    for i, value in enumerate(bad):
        path = project(basis, root / ('skirt-invalid-%d.3mf' % i), [value])
        assert run('skirt-invalid-%d' % i, path, 'slice', executeEffectiveConfig=True,
                   config={'skirt_start_angle': '0'})[0] != 0
        path = project(basis, root / ('skirt-other-invalid-%d.3mf' % i), ['-135'], second=value)
        assert run('skirt-other-invalid-%d' % i, path, 'slice', selectedPlateId='1',
                   executeEffectiveConfig=True)[0] != 0
    conflict = project(basis, root / 'skirt-conflict.3mf', ['-135', '0'])
    assert run('skirt-conflict', conflict, 'slice', executeEffectiveConfig=True)[0] != 0
    unknown = project(basis, root / 'skirt-unknown.3mf', ['-135'], unknown=True)
    assert run('skirt-unknown', unknown, 'slice', executeEffectiveConfig=True)[0] != 0
    multi = project(basis, root / 'skirt-multi.3mf', ['-135'], second='45')
    assert success('skirt-unselected', multi)['project']['blockingFeatures']
    for pid, expected in [('1', -135), ('2', 45)]:
        out = success('skirt-selected-' + pid, multi, selectedPlateId=pid)['project']
        assert not out['blockingFeatures'] and float(out['config']['skirt_start_angle']) == expected


class FixtureTests(unittest.TestCase):
    def test_preservation_duplicate_and_unknown(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); src = root / 'basis.3mf'
            with zipfile.ZipFile(src, 'x') as z:
                z.writestr('Metadata/model_settings.config', '<config><plate><metadata key="plater_id" value="1"/><metadata key="skirt_start_angle" value="77"/><model_instance/><model_instance/></plate></config>')
                z.writestr('3D/3dmodel.model', b'unchanged model bytes')
            original = src.read_bytes()
            for i, values in enumerate([['-135'], ['-135', '0'], ['-135', '-135.0'], ['nan'], ['']]):
                p = project(src, root / ('case%d.3mf' % i), values, unknown=True)
                with zipfile.ZipFile(p) as z:
                    plate = ET.fromstring(z.read('Metadata/model_settings.config')).find('plate')
                    self.assertEqual([e.get('value') for e in plate if e.get('key') == 'skirt_start_angle'], values)
                    self.assertEqual(z.read('3D/3dmodel.model'), b'unchanged model bytes')
                    self.assertEqual(len(plate.findall("metadata[@key='future_skirt_semantics']")), 1)
            multi = project(src, root / 'multi.3mf', ['-135'], second='181')
            with zipfile.ZipFile(multi) as z:
                plates = ET.fromstring(z.read('Metadata/model_settings.config')).findall('plate')
                self.assertEqual([p.find("metadata[@key='skirt_start_angle']").get('value') for p in plates], ['-135', '181'])
                self.assertEqual([len(p.findall('model_instance')) for p in plates], [1, 1])
            self.assertEqual(src.read_bytes(), original)

if __name__ == '__main__':
    unittest.main()

import unittest
import tempfile
import zipfile
from pathlib import Path
import numpy as np
from color_material_distill import inside_polygon, material_labels, remove_isolated_faces, validate_alignment, export_painted_project


class MaterialDistillTests(unittest.TestCase):
    def test_shadow_does_not_create_new_coat_or_skin_material(self):
        centers = np.array([[15,15,0], [15,15,0], [16,0,30], [16,0,30]], float)
        rgb = np.array([[230,230,230],[90,90,90],[225,180,155],[145,102,76]], np.uint8)
        labels, _, _ = material_labels(centers, rgb, np.array([0,5,3,5]), {'base_top_z_mm':-41.7,'features':[]})
        np.testing.assert_array_equal(labels, [2,2,0,0])

    def test_black_hair_white_coat_skin_and_base_do_not_collapse(self):
        c = np.array([[0,0,48],[15,20,0],[15,0,30],[0,0,-48]],float)
        rgb = np.array([[40,40,40],[225,225,224],[211,160,135],[100,100,100]],np.uint8)
        labels, _, _ = material_labels(c,rgb,np.array([1,0,3,5]),{'base_top_z_mm':-41.7,'features':[]})
        np.testing.assert_array_equal(labels,[1,2,0,5])

    def test_feature_locks_and_open_faces_survive_unanimous_neighbours(self):
        labels=np.array([3,3,1,1,1,1],np.uint8); saved=labels.copy()
        a=np.array([0,0,0,1,1]);b=np.array([2,3,4,4,5])
        out=remove_isolated_faces(labels,np.array([True,False,True,True,True,True]),a,b)
        np.testing.assert_array_equal(out,labels)
        out=remove_isolated_faces(labels,np.array([False,False,True,True,True,True]),a,b)
        self.assertEqual(out[0],1);self.assertEqual(out[1],3)
        np.testing.assert_array_equal(labels,saved)

    def test_geometry_alignment_rejects_wrong_face_order(self):
        v=np.array([[0,0,0],[1,0,0],[0,1,0],[0,0,1]],float)
        f=np.array([[0,1,2],[0,2,3]]);centered=v-.5
        self.assertLess(validate_alignment(v,f,centered,f),1e-8)
        with self.assertRaises(ValueError):validate_alignment(v,f,centered,f[::-1])

    def test_front_windows_do_not_protect_back_of_head(self):
        feature={'name':'brow','kind':'brow','min_x_mm':8,'dark_threshold':155,'polygon_yz_mm':[[-2,32],[2,32],[2,36],[-2,36]]}
        c=np.array([[15,0,34],[-15,0,34]],float)
        _,lock,_=material_labels(c,np.array([[80,65,55],[40,40,40]]),np.array([5,1]),{'base_top_z_mm':-41.7,'features':[feature]})
        np.testing.assert_array_equal(lock,[True,False])

    def test_mouth_window_is_not_a_solid_lip_mask(self):
        feature={'name':'mouth','kind':'mouth','min_x_mm':10,'polygon_yz_mm':[[-5,22],[5,22],[5,28],[-5,28]]}
        c=np.array([[15,0,25]]*4,float)
        rgb=np.array([[216,169,143],[175,86,75],[230,220,211],[45,25,25]],np.uint8)
        labels,lock,_=material_labels(c,rgb,np.array([3,3,0,1]),{'base_top_z_mm':-41.7,'features':[feature]})
        np.testing.assert_array_equal(labels,[0,3,2,1])
        self.assertTrue(lock.all())

    def test_two_project_variants_keep_geometry_metadata_and_source_archive_readable(self):
        xml=b'<model><mesh><vertices><vertex x="1" y="2" z="3"/></vertices><triangles><triangle v1="0" v2="0" v3="0" paint_color="4"/></triangles></mesh></model>'
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);name='3D/model.model';source=root/'source.3mf'
            with zipfile.ZipFile(source,'w') as archive:
                archive.writestr(name,xml);archive.writestr('Metadata/project_settings.config',b'{"keep":"exact"}')
            with zipfile.ZipFile(source) as archive:
                export_painted_project(archive,name,np.array([1]),root/'first.3mf')
                export_painted_project(archive,name,np.array([3]),root/'second.3mf')
                self.assertEqual(archive.read(name),xml)
            with zipfile.ZipFile(root/'second.3mf') as result:
                self.assertEqual(result.read(name),xml.replace(b'paint_color="4"',b'paint_color="1C"'))
                self.assertEqual(result.read('Metadata/project_settings.config'),b'{"keep":"exact"}')


if __name__=='__main__':unittest.main()

import unittest
import tempfile
import json
import hashlib
from pathlib import Path
import numpy as np
from color_automatic_features import make_features, expand_feature_windows, load_features
from color_material_distill import material_labels


class AutomaticFeaturesTests(unittest.TestCase):
    schema=['background','face','rb','lb','re','le','imouth','llip','ulip','hair','cloth']

    def test_unknown_low_confidence_and_clothes_never_become_features(self):
        labels=np.array([-1,4,4,10,9]);scores=np.array([1,.89,.95,1,1])
        _,masks=make_features(labels,scores,self.schema)
        union=np.logical_or.reduce(list(masks.values()))
        np.testing.assert_array_equal(union,[False,False,True,False,False])

    def test_mouth_union_and_eye_sides_are_distinct(self):
        labels=np.array([2,3,4,5,6,7,8]);scores=np.ones(7)
        features,masks=make_features(labels,scores,self.schema)
        self.assertEqual(int(masks['mouth'].sum()),3)
        self.assertFalse(np.any(masks['eye-re'] & masks['eye-le']))
        self.assertTrue(all('polygon_yz_mm' not in f and 'min_x_mm' not in f for f in features))

    def test_empty_recognition_stays_empty(self):
        _,masks=make_features(np.full(4,-1),np.zeros(4),self.schema)
        self.assertFalse(any(mask.any() for mask in masks.values()))

    def test_invalid_confidence_and_labels_fail(self):
        with self.assertRaises(ValueError):make_features(np.array([30]),np.ones(1),self.schema)
        with self.assertRaises(ValueError):make_features(np.array([4]),np.array([np.nan]),self.schema)

    def test_face_masks_do_not_depend_on_manual_coordinates(self):
        features,masks=make_features(np.array([2,4,6]),np.ones(3),self.schema)
        profile={'base_top_z_mm':-41.7,'features':features}
        centers=np.array([[10,0,35]]*3,float);rgb=np.array([[130,100,85],[30,30,30],[200,120,110]])
        first,_,_=material_labels(centers,rgb,np.array([5,1,3]),profile,masks)
        for f in features:
            f['polygon_yz_mm']=[[1000,1000],[1001,1000],[1000,1001]];f['min_x_mm']=1000000
        second,_,_=material_labels(centers,rgb,np.array([5,1,3]),profile,masks)
        np.testing.assert_array_equal(first,second)

    def test_candidate_margin_is_bounded_and_cannot_cross_hair_or_disconnected_surfaces(self):
        mask={'eye':np.array([True,False,False,False,False])}
        centers=np.array([[0,0,0],[.1,0,0],[.2,0,0],[.5,0,0],[.01,0,0]])
        semantic=np.array([4,-1,9,-1,-1]);a=np.array([0,1,0]);b=np.array([1,2,3])
        result=expand_feature_windows(mask,semantic,self.schema,centers,a,b)['eye']
        np.testing.assert_array_equal(result,[True,True,False,False,False])

    def test_candidate_margin_is_translation_and_rotation_invariant(self):
        mask={'eye':np.array([True,False,False])};c=np.array([[0,0,0],[.1,0,0],[.2,0,0]])
        sem=np.array([4,-1,-1]);a=np.array([0,1]);b=np.array([1,2])
        first=expand_feature_windows(mask,sem,self.schema,c,a,b)['eye']
        second=expand_feature_windows(mask,sem,self.schema,c[:,[2,0,1]]+100,a,b)['eye']
        np.testing.assert_array_equal(first,second)

    def test_cached_regions_reject_other_models_or_changed_arrays(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            np.savez(root/'regions.npz',semantic=np.array([2,3,4,5,6]),confidence=np.ones(5))
            report={'source_sha256':'source','render_geometry_id':'geometry','manual_feature_windows':False,
                'regions_sha256':hashlib.sha256((root/'regions.npz').read_bytes()).hexdigest(),
                'label_schema':self.schema,'seconds':1}
            (root/'result.json').write_text(json.dumps(report),encoding='utf-8')
            _,masks,_=load_features(root,'source','geometry',5)
            self.assertTrue(all(m.any() for m in masks.values()))
            with self.assertRaises(ValueError):load_features(root,'other-source','geometry',5)
            with self.assertRaises(ValueError):load_features(root,'source','other-geometry',5)
            with self.assertRaises(ValueError):load_features(root,'source','geometry',6)
            np.savez(root/'regions.npz',semantic=np.array([2,3,4,5,6]),confidence=np.zeros(5))
            with self.assertRaises(ValueError):load_features(root,'source','geometry',5)


if __name__=='__main__':unittest.main()

"""Eye geometry and surface association tests: no ML or network needed."""
import copy
import unittest
import numpy as np
import local_eye_landmarks as eyes
import local_semantic_request as request


class EyeHintsTests(unittest.TestCase):
    def points(self):
        p = np.zeros((478, 2))
        angles = np.linspace(np.pi, -np.pi, 16, endpoint=False)
        p[eyes.EYES[0][0]] = np.column_stack((50+25*np.cos(angles), 50-9*np.sin(angles)))
        p[468] = [50, 50]
        p[[469, 470, 471, 472]] = [[59,50], [50,59], [41,50], [50,41]]
        return p

    def test_iris_is_round_and_clipped_by_eyelids(self):
        aperture, iris, width = eyes.eye_masks(self.points(), (128,128), eyes.EYES[0])
        self.assertEqual(width, 50)
        self.assertFalse(np.any(iris & ~aperture))
        self.assertTrue(iris[50,50])
        self.assertFalse(iris[50,64])
        self.assertFalse(iris[38,50])
        self.assertTrue(aperture[50,64])

    def test_closed_nonfinite_and_off_center_eyes_provide_no_hint(self):
        for change in ('closed','nan','center'):
            p = self.points()
            if change == 'closed': p[eyes.EYES[0][0],1] = 50
            if change == 'nan': p[468,0] = np.nan
            if change == 'center': p[468] = [90,90]
            self.assertIsNone(eyes.eye_masks(p,(128,128),eyes.EYES[0]))

    def test_best_view_does_not_union_competing_iris_masks_or_other_subject(self):
        regions = [{'subject_id':'one','label':'re','samples':[[i] for i in range(20)]},
                   {'subject_id':'two','label':'re','samples':[[i] for i in range(100,120)]}]
        observations = [(20,np.arange(20),np.arange(4,10)), (40,np.arange(20),np.arange(7,13)),
                        (90,np.arange(200,220),np.arange(207,213))]
        result = eyes.associate(observations,regions)
        self.assertEqual(len(result),1)
        self.assertEqual(result[0]['iris_faces'], list(range(7,13)))
        request._validate_eye_details(result, regions)

    def test_surface_hints_reject_faces_outside_verified_eye_and_injected_fields(self):
        regions = [{'subject_id':'one','label':'re','samples':[[i] for i in range(20)]}]
        hint = {'subject_id':'one','label':'re','aperture_faces':list(range(20)),'iris_faces':list(range(7,13))}
        for change in ('outside','duplicate','subject','authority','white'):
            h = copy.deepcopy(hint)
            if change == 'outside': h['iris_faces'] = [500]
            if change == 'duplicate': h['iris_faces'] = [7,7]
            if change == 'subject': h['subject_id'] = 'two'
            if change == 'authority': h['confirmed'] = True
            if change == 'white': h['label'] = 'eye_white'
            with self.assertRaises(request.RequestError): request._validate_eye_details([h],regions)

    def test_resolved_frontal_eye_with_unknown_gaps_beats_small_occluded_profile(self):
        regions = [{'subject_id':'one','label':'le','samples':[[i] for i in range(40)]}]
        # The frontal aperture has unknown surrounding faces. They must never
        # enter the returned hint, while its well resolved iris remains useful.
        observations = [(60,np.arange(60),np.arange(12,32)), (25,np.arange(30),np.arange(4,20))]
        result = eyes.associate(observations,regions)
        self.assertEqual(result[0]['iris_faces'], list(range(12,32)))
        self.assertEqual(result[0]['aperture_faces'], list(range(40)))
        request._validate_eye_details(result,regions)


if __name__ == '__main__':
    unittest.main()

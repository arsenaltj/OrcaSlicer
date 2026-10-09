import unittest
import numpy as np
from local_semantic_views import coarse_cameras, focus_camera, parent_cameras
from local_semantic_render import project


class CameraTests(unittest.TestCase):
    def test_all_coarse_views_fit_all_native_vertices_without_a_front_assumption(self):
        v = np.array([[101, -5, 93], [-12, 17, 0], [3, 8, 14], [80, 11, 80]], dtype=np.float32)
        cameras = coarse_cameras(v)
        self.assertEqual(len(cameras), 8)
        for camera in cameras:
            xy = project(v, camera.basis, camera.center, camera.half_height, camera.size)[:, :2]
            self.assertTrue(((xy > 0) & (xy < camera.size)).all())
        self.assertEqual([c.describe() for c in cameras], [c.describe() for c in coarse_cameras(v)])

    def test_translation_moves_camera_without_changing_its_direction_or_extent(self):
        v = np.array([[0, 0, 0], [4, 0, 0], [0, 3, 8]], dtype=np.float32)
        shift = np.array([100, 200, -50], dtype=np.float32)
        for a, b in zip(coarse_cameras(v), coarse_cameras(v+shift)):
            np.testing.assert_allclose(b.center-a.center, shift)
            np.testing.assert_array_equal(a.basis, b.basis)
            self.assertEqual(a.half_height, b.half_height)

    def test_crop_depth_uses_only_selected_visible_surface(self):
        camera = coarse_cameras(np.array([[-1, -1, -1], [1, 1, 1]], dtype=np.float32))[0]
        ids = np.full((512, 512), -1, dtype=np.int32)
        depth = np.full((512, 512), np.inf)
        selected = np.zeros((512, 512), dtype=bool)
        ids[240:272, 240:272] = 7; depth[240:272, 240:272] = .75; selected[240:272, 240:272] = True
        selected[:10] = True  # Unobserved background has no say in crop depth.
        result = focus_camera(camera, [230, 230, 282, 282], ids, depth, selected, 'focus')
        self.assertIsNotNone(result)
        np.testing.assert_allclose((result.center-camera.center) @ camera.basis.T, [0, 0, .75], atol=1e-12)
        self.assertLess(result.half_height, camera.half_height)

    def test_tiny_or_unobserved_faces_do_not_create_guessed_crops(self):
        camera = coarse_cameras(np.array([[0, 0, 0], [1, 1, 1]], dtype=np.float32))[0]
        ids = np.full((512, 512), -1, dtype=np.int32)
        depth = np.full((512, 512), -np.inf)
        selected = np.ones((512, 512), dtype=bool)
        self.assertIsNone(focus_camera(camera, [0, 0, 2, 2], ids, depth, selected, 'tiny'))
        self.assertIsNone(focus_camera(camera, [0, 0, 50, 50], ids, depth, selected, 'empty'))

    def test_parent_views_require_single_subject_reliable_current_face_evidence(self):
        vertices=np.array([[0,y,z] for y in range(6) for z in range(6)
                           for _,y,z in ((0,y,z),(0,y+1,z),(0,y,z+1))],dtype=np.float64)
        faces=np.arange(len(vertices)).reshape(-1,3)
        regions=[dict(subject_id='current',label='face',samples=[[i,.99,.99,3,2] for i in range(len(faces))])]
        cameras=parent_cameras(vertices,faces,regions)
        self.assertEqual(len(cameras),4)
        self.assertEqual(len({c.name for c in cameras}),4)
        for a in cameras:
            np.testing.assert_allclose(a.basis@a.basis.T,np.eye(3),atol=1e-12)
            self.assertEqual(a.size,768)
            for b in cameras:
                if a is not b:
                    self.assertLess(float(a.basis[2]@b.basis[2]),np.cos(np.radians(5)))
        shift=np.array([100.,-50.,200.])
        for a,b in zip(cameras,parent_cameras(vertices+shift,faces,regions)):
            np.testing.assert_allclose(b.center-a.center,shift)
            np.testing.assert_allclose(a.basis,b.basis)
            self.assertAlmostEqual(a.half_height,b.half_height)
        self.assertEqual(parent_cameras(vertices,faces,[]),[])
        self.assertEqual(parent_cameras(vertices,faces,regions+[dict(regions[0],subject_id='other')]),[])
        self.assertEqual(parent_cameras(vertices,faces,[dict(regions[0],label='le')]),[])
        self.assertEqual(parent_cameras(vertices,faces,[dict(regions[0],samples=[[i,.99,.99,3,1] for i in range(len(faces))])]),[])


if __name__ == '__main__':
    unittest.main()

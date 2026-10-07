"""Deterministic textured geometry fixtures; no model or provider access."""
from dataclasses import replace
from unittest.mock import patch
import unittest

import numpy as np

import local_brow_boundary as boundary
import local_face_landmarks as landmarks


class BrowBoundaryTests(unittest.TestCase):
    def fixture(self, family='a', uniform=False):
        yy, xx = np.indices((64, 64))
        ids = (yy // 4 * 16 + xx // 4).astype(np.int32)
        rgb = np.full((64, 64, 3), [214, 175, 155], dtype=np.uint8)
        if not uniform:
            rgb[24:28, 20:44] = [50, 35, 25]
        points = np.zeros((478, 2))
        contour = landmarks.EYEBROWS['lb']
        corners = [[16, 24], [48, 24], [48, 36], [16, 36]]
        points[contour] = [corners[0], [24,24], [32,24], [40,24], corners[1],
                           corners[2], [40,36], [32,36], [24,36], corners[3]]
        mask = landmarks.polygon_mask(points[contour], ids.shape)
        faces = np.unique(ids[mask])
        core = set(np.unique(ids[24:28, 20:44]).tolist())
        projection = boundary.Projection(rgb, ids, np.full((64,64,3), [0.2,0.3,0.5]),
                                          np.stack((xx/64, yy/64), axis=-1), np.ones((64,64), bool),
                                          np.array([0,0]), points)
        view = landmarks.FaceView(family, points, np.zeros((478,3)), np.ones(478,bool),
                                  100., .1, np.arange(256), np.full(256,16), np.arange(256),
                                  {'lb': (faces, np.ones(len(faces)))}, {}, 60., projection)
        neighbors = np.full((256, 3), -1, dtype=np.int32)
        for face in range(256):
            if face % 16 > 0: neighbors[face,0] = face-1
            if face % 16 < 15: neighbors[face,1] = face+1
            if face < 240: neighbors[face,2] = face+16
        return view, faces, core, set(range(256)), neighbors

    def refine(self, views, faces, core, legal, neighbors):
        return boundary.refine('lb', views, faces, core, legal, neighbors,
                               landmarks.EYEBROWS['lb'], landmarks.polygon_mask, landmarks.fuse_masks)

    def test_source_boundary_removes_skin_strip_and_retains_traceable_brow(self):
        a, faces, core, legal, neighbors = self.fixture()
        b = replace(a, family='b')
        result = self.refine([a,b], faces, core, legal, neighbors)
        self.assertTrue(result['refined'])
        self.assertEqual(set(result['faces']), core)
        self.assertTrue(set(result['faces']) < set(faces))
        for row in result['audit']['views']:
            self.assertEqual(row['status'], 'SOURCE_BOUNDARY_REFINED')
            for witness in row['face_witnesses']:
                x,y = witness['pixel']
                self.assertEqual(witness['face_id'], int(a.boundary.ids[y,x]))
                np.testing.assert_allclose(witness['source_uv'], [x/64,y/64])
                np.testing.assert_allclose(witness['barycentric'], [.2,.3,.5])

    def test_unseparable_color_keeps_geometry_as_risk(self):
        a, faces, core, legal, neighbors = self.fixture(uniform=True)
        result = self.refine([a,replace(a,family='b')], faces, core, legal, neighbors)
        self.assertFalse(result['refined'])
        np.testing.assert_array_equal(result['faces'], faces)
        self.assertEqual(result['audit']['views'][0]['status'], 'BROW_SOURCE_COLOR_UNSEPARABLE')

    def test_absent_texture_or_semantic_seed_does_not_fabricate_a_boundary(self):
        a, faces, core, legal, neighbors = self.fixture()
        for views,seeds in (([replace(a,boundary=None),replace(a,family='b',boundary=None)],core),
                            ([a,replace(a,family='b')],set())):
            result = self.refine(views,faces,seeds,legal,neighbors)
            self.assertFalse(result['refined'])
            np.testing.assert_array_equal(result['faces'],faces)

    def test_repeated_views_do_not_supply_source_boundary_support(self):
        a, faces, core, legal, neighbors = self.fixture()
        self.assertFalse(self.refine([a,a],faces,core,legal,neighbors)['refined'])

    def test_source_boundary_never_accepts_forbidden_face(self):
        a, faces, core, legal, neighbors = self.fixture()
        blocked = min(core)
        result = self.refine([a,replace(a,family='b')],faces,core,legal-{blocked},neighbors)
        self.assertTrue(result['refined'])
        self.assertNotIn(blocked, result['faces'])

    def test_search_cannot_cross_more_than_two_topology_rings(self):
        neighbors=np.array([[1,-1,-1],[0,2,-1],[1,3,-1],[2,4,-1],[3,-1,-1]])
        self.assertEqual(boundary._reachable({0},set(range(5)),neighbors,boundary.MAX_HOPS), {0,1,2})
        self.assertEqual(boundary._reachable({0}, {0,2,3,4}, neighbors, boundary.MAX_HOPS), {0})

    def test_installed_opencv_versions_and_parameters_join_identity(self):
        with patch.object(boundary.importlib.metadata, 'version', return_value='fixture-opencv'):
            identity = boundary.runtime_identity()
        self.assertEqual(identity['iterations'], 5)
        self.assertEqual(identity['max_hops'], 2)
        self.assertTrue(all(version == 'fixture-opencv' for version in identity['packages'].values()))


if __name__ == '__main__':
    unittest.main()

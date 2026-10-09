import unittest
from unittest.mock import patch
import numpy as np
from beauty_leaf_domain import LeafKey
from local_leaf_boundaries import AnalyticVisibility, BoundaryView, boundary_band, clip_triangle, area, refine_root, refinement_roots, smooth_lid, source_brow_contour


class BoundaryTests(unittest.TestCase):
    def test_visibility_pruning_preserves_exact_depth_proof_and_tolerated_contacts(self):
        rng = np.random.default_rng(7481)
        triangles = rng.uniform(.1,7.9,(150,3,3))
        # Include subpixel, almost touching, shared-edge and degenerate faces.
        triangles[:5] = [[[1,1,0],[1.001,1,0],[1,1.001,0]],
                         [[1.00100001,1,-1],[1.01,1,-1],[1.00100001,1.01,-1]],
                         [[1,1,-1],[1.001,1,-1],[1,1.001,-1]],
                         [[1,1,0],[1,1,0],[1,1,0]],
                         [[1.001,1,-2],[2,1,-2],[1.001,2,-2]]]
        vertices = triangles.reshape(-1,3)
        faces = np.arange(len(vertices)).reshape(-1,3)
        visibility = AnalyticVisibility(vertices,faces,self.views()[0].transform,(8,8))
        actual = [visibility.fraction(i,t[:,:2],t) for i,t in enumerate(triangles)]
        # Disable only the new broad phase; replay the original all-bucket
        # narrow phase in its original order, including sliver tolerance.
        visibility.lower[:] = -np.inf
        visibility.upper[:] = np.inf
        expected = [visibility.fraction(i,t[:,:2],t) for i,t in enumerate(triangles)]
        np.testing.assert_array_equal(actual,expected)

    def test_nonoverlapping_subpixel_faces_do_not_require_pairwise_clipping(self):
        corners = np.array([[.1,.1,0],[.2,.1,0],[.1,.2,0]])
        triangles = np.array([corners+[x*.3,y*.3,0] for x in range(20) for y in range(20)])
        vertices = triangles.reshape(-1,3)
        faces = np.arange(len(vertices)).reshape(-1,3)
        visibility = AnalyticVisibility(vertices,faces,self.views()[0].transform,(8,8))
        with patch('local_leaf_boundaries.clip_triangle',wraps=clip_triangle) as clipping:
            self.assertEqual(visibility.fraction(0,corners[:,:2],corners),1.)
            self.assertEqual(clipping.call_count,1) # Viewport clip only; 399 disjoint occluders.

    def views(self):
        transform = np.array([[1,0],[0,1],[0,0],[0,0]], dtype=float)
        contour = np.array([[-1,-1],[4,-1],[4,9],[-1,9]], dtype=float)
        return [BoundaryView(name, transform, contour, {0}, 1.) for name in ('front','oblique')]

    def test_analytic_subpixel_coverage(self):
        triangle = np.array([[0,0],[.5,0],[0,.5]])
        polygon = np.array([[0,0],[.25,0],[.25,1],[0,1]])
        self.assertAlmostEqual(area(clip_triangle(polygon, triangle)) / area(triangle), .75)

    def test_split_only_crossing_and_preserve_full_coverage(self):
        corners = np.array([[0,0,0],[8,0,0],[0,8,0]], dtype=float)
        result, status, error = refine_root(0,corners,self.views(),True,200)
        self.assertEqual(status, 'BOUNDARY_REFINED')
        self.assertLessEqual(error, 1.)
        self.assertTrue(any(keep for _,keep in result))
        self.assertTrue(any(not keep for _,keep in result))
        self.assertEqual(sum(4**-key.depth for key,_ in result), 1.)
        self.assertTrue(all(key.depth <= 4 for key,_ in result))

    def test_duplicate_view_budget_precision_fallback(self):
        corners = np.array([[0,0,0],[8,0,0],[0,8,0]], dtype=float)
        duplicate = [self.views()[0], self.views()[0]]
        result,status,_ = refine_root(0,corners,duplicate,True,200)
        self.assertEqual(result, [(LeafKey(0),True)])
        self.assertEqual(status, 'R4_LOCAL_VIEW_FALLBACK')
        self.assertIn('BUDGET', refine_root(0,corners,self.views(),True,0)[1])
        self.assertIn('PRECISION', refine_root(0,corners*100,self.views(),True,200)[1])

    def test_one_ring_limit_and_lid_corners(self):
        neighbors = np.array([[1,-1,-1],[0,2,-1],[1,3,-1],[2,-1,-1]])
        self.assertEqual(boundary_band({0,1}, neighbors)[1], {0,1,2})
        t = np.linspace(0,np.pi,9)
        upper = np.column_stack((1-np.cos(t),-np.sin(t)))
        lower = np.column_stack((1-np.cos(t),np.sin(t)))[-2:0:-1]
        points = np.vstack((upper,lower))
        smoothed = smooth_lid(points)
        np.testing.assert_array_equal(smoothed[0],points[0])
        np.testing.assert_array_equal(smoothed[64],points[8])

    def test_internal_core_never_enters_refinement_ring(self):
        neighbors = np.array([[1,2,3],[0,2,3],[0,1,3],[0,1,4],[3,-1,-1]])
        self.assertEqual(refinement_roots({0,1,2,3},neighbors,set(range(5))), {3,4})
        self.assertEqual(refinement_roots({0,1,2,3},neighbors,set(range(5)),{4}), {3})

    def test_opposed_views_keep_the_approved_root(self):
        corners = np.array([[0,0,0],[1,0,0],[0,1,0]], dtype=float)
        views = self.views()
        views[1].contour = views[1].contour + 50
        result,status,_ = refine_root(0,corners,views,True,200)
        self.assertEqual(result, [(LeafKey(0),True)])
        self.assertEqual(status, 'R4_LOCAL_DISAGREEMENT_FALLBACK')

    def test_subpixel_leaf_visibility_uses_geometry_not_pixel_count(self):
        vertices = np.array([[0,0,0],[1,0,0],[0,1,0],[0,0,-1],[1,0,-1],[0,1,-1]],dtype=float)
        faces = np.array([[0,1,2],[3,4,5]])
        transform = self.views()[0].transform
        occlusion = AnalyticVisibility(vertices,faces,transform,(10,10))
        leaf = LeafKey(0,4,0).corners() @ vertices[faces[0]]
        self.assertEqual(occlusion.fraction(0,leaf[:,:2],leaf), 0.)
        visible = LeafKey(1,4,0).corners() @ vertices[faces[1]]
        self.assertEqual(occlusion.fraction(1,visible[:,:2],visible), 1.)
        views = self.views()
        for view in views:
            view.visibility = occlusion
        result,status,_ = refine_root(0,vertices[faces[0]],views,True,200)
        self.assertEqual(result,[(LeafKey(0),True)])
        self.assertEqual(status,'R4_LOCAL_OCCLUSION_FALLBACK')

    def test_source_brow_keeps_the_pigment_and_excludes_forbidden_dark_pixels(self):
        from local_brow_boundary import Projection
        rgb = np.full((64,100,3),[230,185,165],dtype=np.uint8)
        rgb[24:31,20:81] = [40,32,28]
        rgb[45:55,20:81] = [10,10,10]
        ids = np.full((64,100),2,dtype=int)
        ids[24:31,20:81] = 0
        ids[45:55,20:81] = 3
        projection = Projection(rgb,ids,np.zeros((64,100,3)),np.zeros((64,100,2)),
                                np.ones((64,100),dtype=bool),np.zeros(2),np.zeros((478,2)))
        band = np.array([[18,22],[82,22],[82,33],[18,33]],dtype=float)
        contour,audit = source_brow_contour(projection,{0},{0},{2},{0,2},band)
        self.assertGreater(audit['foreground_seed_pixels'],5)
        self.assertLess(contour[:,1].max() / (1024/100),34)
        rgb[:] = [230,185,165]
        with self.assertRaisesRegex(ValueError,'FOREGROUND'):
            source_brow_contour(projection,{0},{0},{2},{0,2},band)


if __name__ == '__main__':
    unittest.main()

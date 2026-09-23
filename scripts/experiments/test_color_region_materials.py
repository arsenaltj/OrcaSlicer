import unittest
import numpy as np
from color_region_materials import oklab,nearest_palette,material_palette,surface_edges,merge_regions,color_clusters


class RegionMaterialsTests(unittest.TestCase):
    def test_color_reference_and_palette_permutation(self):
        values=oklab(np.array([[0,0,0],[255,255,255],[255,0,0]]))
        np.testing.assert_allclose(values[0],0,atol=1e-7)
        np.testing.assert_allclose(values[1],[1,0,0],atol=1e-7)
        np.testing.assert_allclose(values[2],[.62795536,.22486306,.12584630],atol=1e-7)
        palette=np.array([[230,220,210],[30,30,30],[255,255,255]])
        order=np.array([2,0,1])
        np.testing.assert_array_equal(order[nearest_palette(oklab(palette),palette[order])],[0,1,2])

    def test_exact_uv_seam_but_no_nearby_surface_edge(self):
        vertices=np.array([[0,0,0],[1,0,0],[0,1,0],[1,0,0],[0,0,0],[1,-1,0]],float)
        faces=np.array([[0,1,2],[3,4,5]])
        a,b,lengths,unsafe=surface_edges(vertices,faces)
        self.assertEqual(len(a),1);self.assertEqual(float(lengths[0]),1)
        vertices[3:,2]+=.000001
        self.assertEqual(len(surface_edges(vertices,faces)[0]),0)

    def test_saturated_material_keeps_hue_and_neutral_avoids_blue(self):
        palette=np.array([[234,154,146],[149,139,134],[102,140,182]])
        colors=oklab([[180,45,30],[135,137,140]])
        self.assertEqual(material_palette(colors,palette).tolist(),[0,1])
        order=np.array([2,0,1])
        np.testing.assert_array_equal(order[material_palette(colors,palette[order])],[0,1])

    def test_nonmanifold_and_same_direction_seams_excluded(self):
        v=np.array([[0,0,0],[1,0,0],[0,1,0],[0,-1,0],[0,0,1]],float)
        self.assertEqual(len(surface_edges(v,np.array([[0,1,2],[0,1,3]]))[0]),0)
        self.assertEqual(len(surface_edges(v,np.array([[0,1,2],[1,0,3],[1,0,4]]))[0]),0)

    def test_similar_shadow_merges_but_small_saturated_detail_and_locks_stay(self):
        labels=np.array([0,1,2,3]);areas=np.array([100.,1.,1.,1.])
        lab=np.array([[.9,0,0],[.7,.01,0],[.7,.2,0],[.7,0,0]])
        a=np.array([0,0,0]);b=np.array([1,2,3]);weights=np.ones(3)
        lock=np.array([False,False,False,True]);unsafe=np.zeros(4,bool)
        regions,stats=merge_regions(labels,lab,areas,a,b,weights,lock,unsafe)
        self.assertEqual(regions[0],regions[1])
        self.assertNotEqual(regions[0],regions[2]);self.assertNotEqual(regions[0],regions[3])
        self.assertEqual(stats['merged_regions'],1)

    def test_disconnected_color_does_not_pool_merge_area(self):
        labels=np.array([0,0,1]);areas=np.array([100,1,1.])
        lab=np.array([[.9,0,0],[.9,0,0],[.75,0,0]])
        r,_=merge_regions(labels,lab,areas,np.array([1]),np.array([2]),np.ones(1),np.zeros(3,bool),np.zeros(3,bool))
        self.assertNotEqual(r[1],r[2])

    def test_region_decisions_are_simultaneous_and_edge_order_independent(self):
        labels=np.arange(3);lab=np.array([[.9,0,0],[.8,0,0],[.7,0,0]])
        area=np.array([100,10,1.]);a=np.array([0,1]);b=np.array([1,2]);w=np.array([10.,1.]);lock=np.zeros(3,bool)
        first,_=merge_regions(labels,lab,area,a,b,w,lock,lock)
        second,_=merge_regions(labels,lab,area,b[::-1],a[::-1],w[::-1],lock,lock)
        np.testing.assert_array_equal(first,second)
        self.assertEqual(first.tolist(),[0,0,1])  # no cascading tiny region into 0

    def test_area_weighting_ignores_triangle_count(self):
        rgb=np.array([[230,210,195],[180,160,145],[20,20,20]],float);areas=np.array([10.,2.,3.])
        labels,_=color_clusters(rgb,areas,count=2)
        repeat=np.repeat(np.arange(3),[10,2,3]);sub,_=color_clusters(rgb[repeat],np.ones(15),count=2)
        np.testing.assert_array_equal(labels[repeat],sub)

    def test_saturated_histogram_roundoff_and_degenerate_input(self):
        rgb=np.full((100,3),255,dtype=np.float32);area=np.full(100,.013,dtype=np.float32)
        labels,_=color_clusters(rgb,area)
        self.assertEqual(len(np.unique(labels)),1)
        with self.assertRaises(ValueError):color_clusters(rgb,np.zeros(100))
        with self.assertRaises(ValueError):color_clusters(rgb+1,area)


if __name__=='__main__':unittest.main()

import unittest
from imagemap_weld_project import weld_xml


class ExactWeldTests(unittest.TestCase):
    def test_independent_uv_indices_and_other_xml_preserved(self):
        source=b'''<model><resources><texture2dgroup id="8"><tex2coord u="0" v="1"/></texture2dgroup><mesh><vertices>
<vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/><vertex x="0" y="1" z="0"/>
<vertex x="1" y="0" z="0"/><vertex x="0" y="0" z="0"/><vertex x="1" y="-1" z="0"/>
</vertices><triangles><triangle v1="0" v2="1" v3="2" pid="8" p1="1" p2="2" p3="3"/>
<triangle v1="3" v2="4" v3="5" pid="8" p1="4" p2="5" p3="6"/></triangles></mesh></resources></model>'''
        updated,checks=weld_xml(source)
        self.assertEqual(checks['vertices_after'],4)
        self.assertIn(b'pid="8" p1="4" p2="5" p3="6"',updated)
        self.assertTrue(checks['exact_triangle_corners_roundtrip'])
        self.assertEqual(checks['unpaired_or_nonmanifold_edges_after'],4)

    def test_degenerate_and_extra_vertex_attributes_rejected(self):
        source=b'<model><vertices><vertex x="0" y="0" z="0"/><vertex x="0" y="0" z="0"/><vertex x="1" y="1" z="1"/></vertices><triangles><triangle v1="0" v2="1" v3="2"/></triangles></model>'
        with self.assertRaises(ValueError):weld_xml(source)
        with self.assertRaises(ValueError):weld_xml(source.replace(b'z="0"',b'z="0" color="red"',1))

    def test_multiple_meshes_and_unrecognized_triangles_rejected(self):
        mesh=b'<vertices><vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/><vertex x="0" y="1" z="0"/></vertices><triangles><triangle v1="0" v2="1" v3="2"/></triangles>'
        with self.assertRaises(ValueError):weld_xml(b'<model>'+mesh+mesh+b'</model>')
        with self.assertRaises(ValueError):weld_xml(mesh.replace(b'v1="0" v2="1"',b'v2="1" v1="0"'))


if __name__=='__main__':unittest.main()

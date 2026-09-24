"""Migrated 24 prototype-v2 behaviors plus restricted product-loader regressions.

All GLBs/images below are synthetic temporary fixtures. No inference or provider.
Native Assimp comparison remains a separate host integration responsibility.
"""
from pathlib import Path
import unittest
import numpy as np

import local_semantic_render as renderer
import local_semantic_transform as transforms


class VisibilityTests(unittest.TestCase):
    def test_occluded_surface_never_receives_front_pixels(self):
        vertices = np.array([[-.8,-.8,0],[.8,-.8,0],[0,.8,0],[-.8,-.8,1],[.8,-.8,1],[0,.8,1]])
        faces = np.array([[0,1,2],[3,4,5]])
        ids, depth, bary = renderer.raster(vertices, faces, np.eye(3), np.zeros(3), 1, 32)
        self.assertEqual(set(ids[ids>=0].tolist()), {1})
        np.testing.assert_allclose(depth[ids>=0], 1, atol=1e-7)
        np.testing.assert_allclose(bary[ids>=0].sum(1), 1, atol=1e-7)
        self.assertTrue((ids<0).any())

    def test_intersecting_triangles_use_pixel_depth_instead_of_face_sort(self):
        vertices = np.array([[-.8,-.8,-1],[.8,-.8,1],[0,.8,0],[-.8,-.8,0],[.8,-.8,0],[0,.8,0]])
        faces = np.array([[0,1,2],[3,4,5]])
        ids, depth, _ = renderer.raster(vertices, faces, np.eye(3), np.zeros(3), 1, 32)
        self.assertEqual(ids[22,10], 1)
        self.assertEqual(ids[22,21], 0)
        self.assertGreater(depth[22,21], 0)
        reversed_ids, _, _ = renderer.raster(vertices, faces[::-1], np.eye(3), np.zeros(3), 1, 32)
        np.testing.assert_array_equal(ids[ids>=0], 1-reversed_ids[ids>=0])

    def test_face_interior_samples_texture_instead_of_vertex_color(self):
        faces = np.array([[0,1,2]])
        uv = np.array([[0,0],[1,0],[0,1]], dtype=float)
        texture = np.zeros((3,3,3), dtype=float)
        texture[1,1] = [1,0,0]
        materials = [renderer.Material(np.ones(3),texture,{"magFilter":9728,"minFilter":9728}, {})]
        ids = np.array([[0,-1]])
        bary = np.array([[[1/3,1/3,1/3],[0,0,0]]])
        rgb = renderer.shade(faces,uv,np.ones((3,3)),materials,np.array([0]),ids,bary)
        np.testing.assert_array_equal(rgb[0,0], [255,0,0])
        np.testing.assert_array_equal(rgb[0,1], [184,184,184])





import base64, io, json, struct, tempfile
from PIL import Image
from unittest import mock


def fixture(path, primitives, materials=None, weights=None, nodes=None, texture=None):
    binary=bytearray(); accessors=[]; views=[]; meshes=[]
    def accessor(values, kind, component=5126, normalized=False):
        dtype={5126:'<f4',5125:'<u4',5123:'<u2',5121:'u1',5122:'<i2'}[component]
        values=np.asarray(values,dtype=dtype)
        binary.extend(b'\0'*((-len(binary))%4))
        start=len(binary); binary.extend(values.tobytes())
        views.append({'buffer':0,'byteOffset':start,'byteLength':len(binary)-start})
        data={'bufferView':len(views)-1,'componentType':component,'count':len(values),'type':kind}
        if normalized:data['normalized']=True
        accessors.append(data);return len(accessors)-1
    for p in primitives:
        attr={'POSITION':accessor(p['positions'],p.get('position_type','VEC3'))}
        for semantic,values in p.get('attributes',{}).items():
            attr[semantic]=accessor(values,'VEC'+str(len(values[0])))
        q={'attributes':attr,'indices':accessor(p.get('indices',[0,1,2]),p.get('index_type','SCALAR'),p.get('index_component',5125),p.get('index_normalized',False))}
        if 'material' in p:q['material']=p['material']
        if 'delta' in p:q['targets']=[{'POSITION':accessor(p['delta'],'VEC3')}]
        meshes.append(q)
    mesh={'primitives':meshes}
    if weights is not None:mesh['weights']=weights
    doc={'asset':{'version':'2.0'},'buffers':[{'byteLength':len(binary)}],'bufferViews':views,'accessors':accessors,'meshes':[mesh],
         'nodes':nodes or [{'mesh':0}],'scenes':[{'nodes':[0]}],'scene':0}
    if materials is not None:doc['materials']=materials
    if texture is not None:
        image=io.BytesIO();Image.fromarray(np.asarray(texture,dtype=np.uint8)).save(image,format='PNG')
        doc['images']=[{'uri':'data:image/png;base64,'+base64.b64encode(image.getvalue()).decode()}]
        doc['textures']=[{'source':0}]
    data=json.dumps(doc).encode();data+=b' '*((-len(data))%4);binary+=b'\0'*((-len(binary))%4)
    path.write_bytes(struct.pack('<4sII',b'glTF',2,28+len(data)+len(binary))+struct.pack('<II',len(data),0x4e4f534a)+data+struct.pack('<II',len(binary),0x004e4942)+binary)
    return path


def rewrite_doc(path, mutate):
    """Rewrite only a synthetic fixture's JSON chunk, preserving BIN bytes."""
    raw=path.read_bytes(); count=struct.unpack_from('<I',raw,12)[0]
    doc=json.loads(raw[20:20+count]); mutate(doc)
    text=json.dumps(doc,separators=(',',':')).encode('utf-8'); text+=b' '*((-len(text))%4)
    tail=raw[20+count:]
    path.write_bytes(struct.pack('<4sII',b'glTF',2,20+len(text)+len(tail))+struct.pack('<II',len(text),0x4e4f534a)+text+tail)
    return path


class LoaderTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name)/'fixture.glb'
        self.tri={'positions':[[0,0,0],[.001,0,0],[0,.001,0]],'material':0}

    def test_nonopaque_modes_are_rejected_before_rasterization(self):
        for mode in ['MASK','BLEND','unknown']:
            with self.subTest(mode=mode):
                p=fixture(self.path,[self.tri],materials=[{'alphaMode':mode,'pbrMetallicRoughness':{'baseColorFactor':[1,0,0,0]}}])
                with self.assertRaisesRegex(ValueError,'OPAQUE'):renderer.load(p)

    def test_opaque_alpha_is_ignored_and_double_sided_is_preserved(self):
        p=fixture(self.path,[self.tri],materials=[{'alphaMode':'OPAQUE','doubleSided':True,'pbrMetallicRoughness':{'baseColorFactor':[1,0,0,0]}}])
        v,f,u,c,m,mi=renderer.load(p)
        np.testing.assert_array_equal(renderer.double_sided_faces(m,mi),[True])
        np.testing.assert_array_equal(m[0].factor,[1,0,0])

    def test_unbaked_morphs_are_rejected_including_mesh_default_weights(self):
        p=fixture(self.path,[dict(self.tri,delta=[[.001,0,0]]*3)],weights=[1],materials=[{}])
        with self.assertRaisesRegex(ValueError,'Morph'):renderer.load(p)
        p=fixture(self.path,[dict(self.tri,delta=[[0,0,0]]*3)],materials=[{}])
        with self.assertRaisesRegex(ValueError,'Morph'):renderer.load(p)

    def test_primitive_indices_cannot_address_following_primitives_or_wrap_negative(self):
        for index in [3,2**32-1]:
            with self.subTest(index=index):
                p=fixture(self.path,[dict(self.tri,indices=[index,1,2]),self.tri],materials=[{}])
                with self.assertRaisesRegex(ValueError,'outside its primitive'):renderer.load(p)

    def test_index_format_and_triangle_count_are_checked(self):
        for patch in [{'index_component':5126},{'index_component':5122},{'index_component':5123,'index_normalized':True},{'indices':[0,1]}]:
            with self.subTest(patch=patch):
                p=fixture(self.path,[dict(self.tri,**patch)],materials=[{}])
                with self.assertRaises(ValueError):renderer.load(p)

    def test_attribute_counts_and_shapes_are_checked(self):
        for patch in [{'attributes':{'COLOR_0':[[1,0,0]]*2}},{'attributes':{'TEXCOORD_0':[[0,0,0]]*3}},{'position_type':'VEC2'}]:
            with self.subTest(patch=patch):
                p=fixture(self.path,[dict(self.tri,**patch)],materials=[{}])
                with self.assertRaises(ValueError):renderer.load(p)

    def test_total_limits_apply_across_primitives(self):
        p=fixture(self.path,[self.tri,self.tri],materials=[{}])
        with mock.patch.object(renderer,'MAX_VERTICES',5):
            with self.assertRaisesRegex(ValueError,'vertex count'):renderer.load(p)
        with mock.patch.object(renderer,'MAX_FACES',1):
            with self.assertRaisesRegex(ValueError,'triangle count'):renderer.load(p)

    def test_nested_transform_and_reflection_preserve_coordinates_and_corner_order(self):
        s=np.sqrt(.5)
        p=fixture(self.path,[self.tri],materials=[{}],nodes=[{'translation':[.01,.02,.03],'children':[1]},{'mesh':0,'rotation':[0,0,s,s],'scale':[-2,3,1]}])
        v,f,*_=renderer.load(p)
        np.testing.assert_allclose(v,[[10,-30,20],[10,-30,18],[7,-30,20]],atol=1e-5)
        np.testing.assert_array_equal(f,[[2,1,0]])

    def test_embedded_srgb_texture_is_decoded_before_linear_factor(self):
        p=fixture(self.path,[dict(self.tri,attributes={'TEXCOORD_0':[[.5,.5]]*3})],
                  materials=[{'pbrMetallicRoughness':{'baseColorFactor':[.5,.5,.5,1],'baseColorTexture':{'index':0}}}],
                  texture=[[[128,128,128]]])
        _,f,u,c,m,mi=renderer.load(p)
        rgb=renderer.shade(f,u,c,m,mi,np.array([[0]]),np.array([[[1/3,1/3,1/3]]]))
        np.testing.assert_array_equal(rgb[0,0],[92,92,92])

    def test_module_import_and_loading_do_not_write_outputs(self):
        p=fixture(self.path,[self.tri],materials=[{}])
        before=set(Path(self.temp.name).iterdir())
        renderer.load(p)
        self.assertEqual(set(Path(self.temp.name).iterdir()),before)
        self.assertNotIn('sys.path.insert',Path(renderer.__file__).read_text(encoding='utf-8'))
        self.assertIn('base-level-only',renderer.FILTER_POLICY)


class RasterRegressionTests(unittest.TestCase):
    def setUp(self):
        self.tri=np.array([[-.8,-.8,0],[.8,-.8,0],[0,.8,0]])
        self.faces=np.array([[0,1,2],[3,4,5]])

    def test_single_sided_backface_does_not_occlude_but_double_sided_can(self):
        vertices=np.concatenate([self.tri,self.tri+[0,0,1]])
        faces=np.array([[0,1,2],[3,5,4]])
        ids,*_=renderer.raster(vertices,faces,np.eye(3),np.zeros(3),1,32)
        self.assertEqual(set(ids[ids>=0]),{0})
        ids,*_=renderer.raster(vertices,faces,np.eye(3),np.zeros(3),1,32,np.array([False,True]))
        self.assertEqual(set(ids[ids>=0]),{1})

    def test_close_surfaces_use_float64_depth_without_far_overwrite(self):
        vertices=np.concatenate([self.tri+[0,0,10000.0002],self.tri+[0,0,10000.0001]])
        for faces,expected in [(self.faces,0),(self.faces[::-1],1)]:
            ids,depth,_=renderer.raster(vertices,faces,np.eye(3),np.zeros(3),1,32)
            self.assertEqual(set(ids[ids>=0]),{expected})
            self.assertEqual(depth.dtype,np.float64)
            np.testing.assert_allclose(depth[ids>=0],10000.0002,atol=1e-10,rtol=0)

    def test_exact_depth_ties_keep_lowest_face_id(self):
        vertices=np.concatenate([self.tri+[0,0,.7],self.tri+[0,0,.7]])
        ids,*_=renderer.raster(vertices,self.faces,np.eye(3),np.zeros(3),1,32)
        self.assertEqual(set(ids[ids>=0]),{0})

    def test_shared_edge_ownership_is_independent_of_traversal_order(self):
        vertices=np.array([[-1,-1,0],[1,-1,0],[1,1,0],[-1,1,0]])
        faces=np.array([[0,1,2],[0,2,3]])
        first,*_=renderer.raster(vertices,faces,np.eye(3),np.zeros(3),1,8)
        reverse,*_=renderer.raster(vertices,faces[::-1],np.eye(3),np.zeros(3),1,8)
        self.assertTrue((first>=0).all())
        np.testing.assert_array_equal(first,1-reverse)

    def test_barycentric_coordinates_reconstruct_pixel_centers_and_depth(self):
        vertices=self.tri+np.array([[0,0,-1],[0,0,1],[0,0,0]])
        ids,depth,bary=renderer.raster(vertices,np.array([[0,1,2]]),np.eye(3),np.zeros(3),1,32)
        y,x=np.nonzero(ids>=0);points=(vertices*bary[y,x,:,None]).sum(1)
        np.testing.assert_allclose(points[:,0],(x+.5)*2/32-1,atol=1e-12)
        np.testing.assert_allclose(points[:,1],1-(y+.5)*2/32,atol=1e-12)
        np.testing.assert_allclose(points[:,2],depth[y,x],atol=1e-12)

    def test_subpixel_geometry_remains_unobserved_and_invalid_camera_rejected(self):
        vertices=np.array([[.001,-.8,0],[.009,-.8,0],[.005,.8,0]])
        ids,*_=renderer.raster(vertices,np.array([[0,1,2]]),np.eye(3),np.zeros(3),1,32)
        self.assertTrue((ids==-1).all())
        for height in [0,-1,np.inf]:
            with self.assertRaises(ValueError):renderer.raster(vertices,np.array([[0,1,2]]),np.eye(3),np.zeros(3),height,32)


class TextureRegressionTests(unittest.TestCase):
    def setUp(self):
        self.texture=np.array([[[1.,0.,0.],[0.,0.,1.]]])
        self.faces=np.array([[0,1,2]])
        self.ids=np.array([[0]])
        self.bary=np.array([[[1/3,1/3,1/3]]])

    def test_linear_rgb_midpoint_encodes_to_srgb_purple(self):
        material=renderer.Material(np.ones(3),self.texture,{'magFilter':9729,'minFilter':9729,'wrapS':33071,'wrapT':33071},{})
        rgb=renderer.shade(self.faces,np.tile([.5,.5],(3,1)),np.ones((3,3)),[material],np.array([0]),self.ids,self.bary)
        np.testing.assert_array_equal(rgb[0,0],[188,0,188])

    def test_bilinear_neighbors_wrap_independently_at_texture_seams(self):
        coords=np.array([[0,.5],[1,.5],[-.5,.5],[.5,.5]])
        sampled=renderer.sample_texture(self.texture,coords,{},True)
        np.testing.assert_allclose(sampled,[[.5,0,.5]]*4)
        clamped=renderer.sample_texture(self.texture,coords,{'wrapS':33071},True)
        np.testing.assert_allclose(clamped,[[1,0,0],[0,0,1],[1,0,0],[.5,0,.5]])
        mirrored=renderer.sample_texture(self.texture,np.array([[0,.5],[1,.5],[2,.5],[-.25,.5]]),{'wrapS':33648},True)
        np.testing.assert_allclose(mirrored,[[1,0,0],[0,0,1],[1,0,0],[1,0,0]])

    def test_texel_centers_and_top_left_origin_do_not_flip_uv(self):
        texture=np.array([[[1.,0.,0.],[0.,1.,0.]],[[0.,0.,1.],[1.,1.,1.]]])
        sampled=renderer.sample_texture(texture,np.array([[.25,.25],[.75,.25],[.25,.75],[.75,.75]]),{},True)
        np.testing.assert_array_equal(sampled,texture.reshape(-1,3))

    def test_minification_selects_declared_base_level_fallback(self):
        # Tiny screen triangle yields >1 texel/pixel; mag NEAREST, min LINEAR mipmap fallback.
        material=renderer.Material(np.ones(3),self.texture,{'magFilter':9728,'minFilter':9987},{})
        uv=np.array([[0,0],[1,0],[.5,1.5]])
        small=np.array([[0,0,0],[.1,0,0],[0,.1,0]])
        large=small*100
        result=renderer.shade(self.faces,uv,np.ones((3,3)),[material],np.array([0]),self.ids,self.bary,small)
        np.testing.assert_array_equal(result[0,0],[188,0,188])
        result=renderer.shade(self.faces,uv,np.ones((3,3)),[material],np.array([0]),self.ids,self.bary,large)
        np.testing.assert_array_equal(result[0,0],[0,0,255])
        with self.assertRaisesRegex(ValueError,'Projected'):renderer.shade(self.faces,uv,np.ones((3,3)),[material],np.array([0]),self.ids,self.bary)

    def test_texture_transform_applies_scale_rotation_then_offset(self):
        coords=renderer._transform_uv(np.array([[.2,.3]]),{'scale':[2,3],'rotation':np.pi/2,'offset':[.1,.2]})
        np.testing.assert_allclose(coords,[[-.8,.6]],atol=1e-12)

    def test_nonfinite_and_overflow_uvs_do_not_become_arbitrary_texel_indices(self):
        for coords in [np.array([[np.nan,0]]),np.array([[np.inf,0]])]:
            with self.assertRaises(ValueError):renderer.sample_texture(self.texture,coords,{},True)
        with self.assertRaisesRegex(ValueError,'Non-finite'):
            renderer._transform_uv(np.array([[1e300,0]]),{'scale':[1e300,1]})


class NativeTransformTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name)/'transform.glb'
        self.tri={'positions':[[0,0,0],[.001,0,0],[0,.001,0]]}

    def test_each_float32_operation_preserves_native_placement_cancellation(self):
        tri={'positions':[[0,-.49997,0],[.0001,-.49997,0],[0,-.49996,.0001]]}
        fixture(self.path,[tri],nodes=[{'mesh':0,'scale':[.1,.1,.1],'translation':[0,.05,0]}])
        vertices,faces,*_=renderer.load(self.path)
        self.assertEqual(vertices.dtype,np.float32); self.assertEqual(faces.dtype,np.int32)
        np.testing.assert_array_equal(vertices[:,2],np.array([.0030025839805603027,.0030025839805603027,.004000961780548096],np.float32))
        # A single final float32 cast is not equivalent near cancellation.
        old=(np.array(tri['positions'],np.float32).astype(float)[:,1]*.1+.05)*1000
        self.assertGreater(np.max(abs(old-vertices[:,2])),1e-6)

    def test_parent_local_values_round_before_matrix_composition(self):
        fixture(self.path,[self.tri],nodes=[{'translation':[10000,0,0],'children':[1]},
                                          {'mesh':0,'translation':[-9999.9999,0,0]}])
        vertices,*_=renderer.load(self.path)
        np.testing.assert_array_equal(vertices,[[0,0,0],[1,0,0],[0,0,1]])

    def test_native_near_identity_skip_is_explicit_not_ideal_gltf(self):
        fixture(self.path,[self.tri],nodes=[{'mesh':0,'translation':[.005,0,0]}])
        vertices,*_=renderer.load(self.path)
        np.testing.assert_array_equal(vertices,[[0,0,0],[1,0,0],[0,0,1]])
        fixture(self.path,[self.tri],nodes=[{'mesh':0,'translation':[.02,0,0]}])
        vertices,*_=renderer.load(self.path)
        np.testing.assert_allclose(vertices[:,0],[20,21,20],atol=2e-6,rtol=0)

    def test_matrix_layout_and_trs_have_the_same_restricted_result(self):
        node={'mesh':0,'translation':[.1,.2,.3],'scale':[2,3,4]}
        fixture(self.path,[self.tri],nodes=[node]); first=renderer.load(self.path)[0]
        matrix=transforms.node_matrix(node).T.reshape(-1).tolist()
        fixture(self.path,[self.tri],nodes=[{'mesh':0,'matrix':matrix}]); second=renderer.load(self.path)[0]
        np.testing.assert_array_equal(first,second)

    def test_nonunit_quaternion_mixed_matrix_and_singular_scale_are_rejected(self):
        matrix=np.eye(4).T.reshape(-1).tolist()
        for extra in [{'rotation':[0,0,0,2]},{'rotation':[0,0,0,0]},
                      {'matrix':matrix,'translation':[0,0,0]},{'scale':[1,0,1]},
                      {'matrix':[1,0,0,.000001,0,1,0,0,0,0,1,0,0,0,0,1]},
                      {'translation':[1e100,0,0]}]:
            with self.subTest(extra=extra):
                fixture(self.path,[self.tri],nodes=[{'mesh':0,**extra}])
                with self.assertRaises(ValueError):renderer.load(self.path)

    def test_mesh_instances_count_towards_resource_limits(self):
        fixture(self.path,[self.tri],nodes=[{'children':[1,2]},{'mesh':0},{'mesh':0,'translation':[1,0,0]}])
        vertices,faces,*_=renderer.load(self.path)
        self.assertEqual(len(vertices),6);self.assertEqual(len(faces),2)
        with mock.patch.object(transforms,'MAX_MESH_INSTANCES',1):
            with self.assertRaisesRegex(ValueError,'instances'):renderer.load(self.path)
        with mock.patch.object(renderer,'MAX_VERTICES',5):
            with self.assertRaises(ValueError):renderer.load(self.path)

    def test_shared_nodes_cycles_depth_and_primitive_instance_limits_are_rejected(self):
        for nodes in [[{'children':[1,1]},{'mesh':0}],[{'children':[1]},{'children':[0]}]]:
            with self.subTest(nodes=nodes):
                fixture(self.path,[self.tri],nodes=nodes)
                with self.assertRaises(ValueError):renderer.load(self.path)
        fixture(self.path,[self.tri],nodes=[{'children':[1]},{'children':[2]},{'mesh':0}])
        with mock.patch.object(transforms,'MAX_DEPTH',1):
            with self.assertRaisesRegex(ValueError,'depth'):renderer.load(self.path)
        fixture(self.path,[self.tri,self.tri])
        with mock.patch.object(transforms,'MAX_PRIMITIVES',1):
            with self.assertRaisesRegex(ValueError,'primitive'):renderer.load(self.path)

    def test_inward_closed_tetrahedron_uses_final_whole_mesh_winding(self):
        outward=np.array([[0,2,1],[0,1,3],[0,3,2],[1,2,3]],np.int32)
        inward=outward[:,[0,2,1]]
        uv=[[0,0],[.2,.3],[.8,.4],[1,1]]
        tetra={'positions':[[0,0,0],[.001,0,0],[0,.001,0],[0,0,.001]],
               'indices':inward.reshape(-1).tolist(),'attributes':{'TEXCOORD_0':uv}}
        fixture(self.path,[tetra])
        vertices,faces,actual_uv,*_=renderer.load(self.path)
        self.assertLess(transforms.native_signed_volume(vertices,inward),0)
        self.assertGreater(transforms.native_signed_volume(vertices,faces),0)
        np.testing.assert_array_equal(faces,outward)
        np.testing.assert_allclose(actual_uv,uv,atol=3e-8,rtol=0)
        # Already outward input is retained, including its original face order.
        tetra['indices']=outward.reshape(-1).tolist();fixture(self.path,[tetra])
        np.testing.assert_array_equal(renderer.load(self.path)[1],outward)

    def test_two_open_instance_triangles_flip_only_after_complete_mesh_assembly(self):
        tri={'positions':[[.1234567,-.49993795,.004],[.224,-.49,.003],[.126,-.35,.075]],
             'attributes':{'TEXCOORD_0':[[0,0],[1,0],[0,1]]}}
        fixture(self.path,[tri],nodes=[{'children':[1,2]},{'mesh':0},{'mesh':0,'translation':[.9,.7,0]}])
        vertices,faces,uv,*_=renderer.load(self.path)
        before=np.array([[0,1,2],[3,4,5]],np.int32)
        self.assertLess(transforms.native_signed_volume(vertices,before),0)
        np.testing.assert_array_equal(faces,[[0,2,1],[3,5,4]])
        np.testing.assert_array_equal(uv,[[0,0],[1,0],[0,1],[0,0],[1,0],[0,1]])
        # The first triangle alone has zero reference volume and must not flip;
        # this protects against normalizing each primitive/instance separately.
        self.assertEqual(float(transforms.native_signed_volume(vertices[:3],before[:1])),0.)

    def test_orientation_statistic_rejects_degeneracy_overflow_and_cancellation(self):
        vertices=np.array([[0,0,0],[1,0,0],[0,1,0],[0,0,1],[1,0,1],[0,1,1]],np.float32)
        opposing=np.array([[0,1,2],[3,4,5],[3,5,4]],np.int32)
        with self.assertRaisesRegex(ValueError,'Unstable'):
            transforms.finalize_winding(vertices,opposing)
        with self.assertRaisesRegex(ValueError,'Degenerate'):
            transforms.finalize_winding(vertices,np.array([[0,0,1]],np.int32))
        with self.assertRaisesRegex(ValueError,'non-finite'):
            transforms.finalize_winding(vertices*np.float32(1e30),np.array([[0,1,2]],np.int32))


class StrictResourceTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name)/'bounded.glb'
        self.tri={'positions':[[0,0,0],[.001,0,0],[0,.001,0]],'material':0,
                  'attributes':{'TEXCOORD_0':[[0,0],[1,0],[0,1]]}}
        self.material={'pbrMetallicRoughness':{'baseColorTexture':{'index':0}}}

    def make(self, textured=False):
        return fixture(self.path,[self.tri],materials=[self.material if textured else {}],
                       texture=[[[10,20,30],[20,30,40]],[[30,40,50],[40,50,60]]] if textured else None)

    def test_quantization_unknown_extensions_and_unbaked_animation_are_rejected(self):
        for mutate in [lambda d:d.update(extensionsUsed=['KHR_mesh_quantization']),
                       lambda d:d.update(extensionsRequired=['EXT_meshopt_compression']),
                       lambda d:d['nodes'][0].update(extensions={'EXT_unknown':{}}),
                       lambda d:d.update(animations=[{}]),lambda d:d.update(skins=[{}]),
                       lambda d:d['meshes'][0]['primitives'][0]['attributes'].update(JOINTS_0=0)]:
            self.make();rewrite_doc(self.path,mutate)
            with self.assertRaises(ValueError):renderer.load(self.path)

    def test_accessor_alignment_sparse_ranges_and_unused_attributes_are_checked(self):
        mutations=[lambda d:d['accessors'][0].update(sparse={}),
                   lambda d:d['bufferViews'][0].update(byteOffset=1),
                   lambda d:d['accessors'][0].update(byteOffset=4),
                   lambda d:d['bufferViews'][0].update(buffer=False),
                   lambda d:d['accessors'][0].update(count=True),
                   lambda d:d['accessors'][0].update(normalized=True)]
        for mutate in mutations:
            self.make();rewrite_doc(self.path,mutate)
            with self.assertRaises(ValueError):renderer.load(self.path)
        bad=dict(self.tri,attributes={'NORMAL':[[float('nan'),0,0]]*3})
        fixture(self.path,[bad],materials=[{}])
        with self.assertRaisesRegex(ValueError,'Non-finite'):renderer.load(self.path)

    def test_image_limits_are_checked_before_decoding_or_linear_allocation(self):
        self.make(True)
        for key,limit in [('MAX_IMAGE_BYTES',1),('MAX_IMAGE_PIXELS',3),('MAX_TOTAL_IMAGE_PIXELS',3),('MAX_IMAGES',0),('MAX_IMAGE_DIMENSION',1)]:
            with self.subTest(key=key),mock.patch.object(renderer,key,limit):
                with self.assertRaises(ValueError):renderer.load(self.path)

    def test_distinct_images_use_cumulative_budget_and_reuse_does_not(self):
        fixture(self.path,[self.tri,self.tri],materials=[self.material],texture=[[[255,0,0]]])
        with mock.patch.object(renderer,'MAX_TOTAL_IMAGE_PIXELS',1):renderer.load(self.path)
        def duplicate(d):
            d['images'].append(dict(d['images'][0]));d['textures'].append({'source':1})
            d['materials'].append({'pbrMetallicRoughness':{'baseColorTexture':{'index':1}}})
            d['meshes'][0]['primitives'][1]['material']=1
        rewrite_doc(self.path,duplicate)
        with mock.patch.object(renderer,'MAX_TOTAL_IMAGE_PIXELS',1):
            with self.assertRaisesRegex(ValueError,'pixel limit'):renderer.load(self.path)

    def test_external_images_and_misdeclared_mime_are_rejected(self):
        for value in ['https://example.invalid/no.png','file:///private.png','../texture.png']:
            self.make(True);rewrite_doc(self.path,lambda d:d['images'][0].update(uri=value))
            with self.assertRaises(ValueError):renderer.load(self.path)
        self.make(True)
        rewrite_doc(self.path,lambda d:d['images'][0].update(mimeType='image/jpeg'))
        with self.assertRaisesRegex(ValueError,'MIME'):renderer.load(self.path)

    def test_duplicate_json_keys_and_excessive_file_size_are_rejected(self):
        self.make();raw=self.path.read_bytes();count=struct.unpack_from('<I',raw,12)[0]
        doc=raw[20:20+count].rstrip();doc=b'{"asset":{"version":"2.0"},'+doc[1:];doc+=b' '*((-len(doc))%4)
        tail=raw[20+count:]
        self.path.write_bytes(struct.pack('<4sII',b'glTF',2,20+len(doc)+len(tail))+struct.pack('<II',len(doc),0x4e4f534a)+doc+tail)
        with self.assertRaisesRegex(ValueError,'Duplicate'):renderer.load(self.path)
        self.make()
        with mock.patch.object(renderer,'MAX_BYTES',64):
            with self.assertRaisesRegex(ValueError,'size'):renderer.load(self.path)

    def test_missing_accessor_definitions_and_nonfinite_metadata_are_rejected(self):
        self.make();rewrite_doc(self.path,lambda d:d.pop('accessors'))
        with self.assertRaises(ValueError):renderer.load(self.path)
        self.make();rewrite_doc(self.path,lambda d:d['nodes'][0].update(translation=[float('nan'),0,0]))
        with self.assertRaisesRegex(ValueError,'Non-finite'):renderer.load(self.path)


if __name__ == '__main__':unittest.main()

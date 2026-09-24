"""Bounded offline base-color renderer for local semantic evidence.

Derived from the independently tested render-prototype v2. Opaque static GLB
subset only; load success is NOT proof of native face order. The host must prove
actual face/corner geometry before consuming labels. Geometry uses native
float32 Z-up millimetres; raster/depth/barycentric calculations use float64.
No inference, camera policy, process launch, file output or import path changes.
"""
import base64
import ctypes
import io
import json
import math
import os
import struct
from pathlib import Path
from typing import NamedTuple

import numpy as np
from PIL import Image
from glb_artifact import Glb, _item, _numbers, _int, MAX_BYTES, MAX_VERTICES, MAX_FACES
from local_semantic_transform import primitives, apply_transform, finalize_winding

RENDERER = 'local-semantic-orthographic-v1'
FILTER_POLICY = 'linear-RGB; pixel-footprint mag/min; base-level-only mipmap fallback; no anisotropy'
MAX_IMAGES = 32
MAX_IMAGE_BYTES = 64 * 1024 * 1024
MAX_IMAGE_PIXELS = 16 * 1024 * 1024
MAX_TOTAL_IMAGE_PIXELS = 32 * 1024 * 1024
MAX_IMAGE_DIMENSION = 8192
_native_raster = None


def _native_raster_kernel():
    """Load only the installed sibling accelerator; absence keeps Python exact."""
    global _native_raster
    if _native_raster is not None:
        return _native_raster[1]
    if os.name != 'nt':
        return None
    path = Path(__file__).resolve().parent / 'local_semantic_raster.dll'
    if not path.exists():
        return None
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 2 * 1024 * 1024:
        raise ValueError('Invalid installed semantic raster accelerator')
    library = ctypes.CDLL(str(path))
    version = library.semantic_raster_abi_version
    version.argtypes = []
    version.restype = ctypes.c_int
    if version() != 1:
        raise ValueError('Unsupported semantic raster accelerator')
    function = library.raster_triangles
    function.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32,
                         ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    function.restype = ctypes.c_int
    _native_raster = (library, function)
    return function


def _extensions(value, allowed=()):
    extensions = value.get('extensions', {})
    if not isinstance(extensions, dict) or set(extensions)-set(allowed):
        raise ValueError('Unsupported extensions in semantic render input')
    return extensions


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result: raise ValueError('Duplicate GLB JSON key')
        result[key] = value
    return result


def _json_shape(value, depth=0):
    if depth > 32: raise ValueError('Excessive GLB JSON depth')
    if isinstance(value, dict):
        for item in value.values(): _json_shape(item, depth+1)
    elif isinstance(value, list):
        for item in value: _json_shape(item, depth+1)
    elif isinstance(value, float) and not math.isfinite(value):
        raise ValueError('Non-finite GLB JSON')


class _SemanticGlb(Glb):
    """Stricter bounded reader; leaves shared generation Glb semantics untouched."""
    def __init__(self, path):
        path = Path(path)
        if not 20 <= path.stat().st_size <= MAX_BYTES: raise ValueError('Invalid GLB file size')
        with path.open('rb') as stream:
            data = stream.read(MAX_BYTES+1)
        if len(data) > MAX_BYTES or len(data) < 20: raise ValueError('Invalid GLB file size')
        magic, version, length = struct.unpack_from('<4sII', data)
        if magic != b'glTF' or version != 2 or length != len(data): raise ValueError('Invalid GLB header')
        chunks, offset = [], 12
        while offset < length:
            if offset+8 > length or len(chunks) >= 2: raise ValueError('Unsupported GLB chunks')
            size, kind = struct.unpack_from('<II', data, offset); offset += 8
            if size % 4 or size > length-offset: raise ValueError('Invalid GLB chunk length')
            chunks.append((kind, memoryview(data)[offset:offset+size])); offset += size
        if len(chunks) != 2 or [c[0] for c in chunks] != [0x4e4f534a, 0x004e4942] or len(chunks[0][1]) > 32*1024*1024:
            raise ValueError('Expected bounded JSON and BIN chunks')
        try:
            self.doc = json.loads(bytes(chunks[0][1]), object_pairs_hook=_unique)
            _json_shape(self.doc)
        except (UnicodeError, RecursionError) as error:
            raise ValueError('Invalid GLB JSON') from error
        if not isinstance(self.doc, dict) or not isinstance(self.doc.get('asset'), dict) or self.doc['asset'].get('version') != '2.0':
            raise ValueError('Only GLB 2.0 is supported')
        _extensions(self.doc); _extensions(self.doc['asset'])
        allowed = {'KHR_materials_unlit', 'KHR_texture_transform'}
        for key in ('extensionsUsed', 'extensionsRequired'):
            values = self.doc.get(key, [])
            if not isinstance(values, list) or any(not isinstance(v, str) for v in values) or len(set(values)) != len(values) or set(values)-allowed:
                raise ValueError('Unsupported GLB extension; quantized geometry must be baked')
        for key in ('animations', 'skins'):
            if self.doc.get(key): raise ValueError('Animations and skins must be baked')
        for key in ('nodes','scenes','meshes','accessors','bufferViews','buffers','materials','textures','samplers','images'):
            values = self.doc.get(key, [])
            if not isinstance(values, list) or len(values) > 65536 or any(not isinstance(v,dict) for v in values):
                raise ValueError('Invalid or excessive GLB definitions')
            if key != 'materials':
                for value in values: _extensions(value)
        if len(self.doc.get('images', [])) > MAX_IMAGES: raise ValueError('Too many image resources')
        for mesh in self.doc.get('meshes', []):
            items = mesh.get('primitives')
            if not isinstance(items, list) or not items or any(not isinstance(p,dict) for p in items):
                raise ValueError('Invalid mesh primitives')
            if 'weights' in mesh or any('targets' in p for p in items): raise ValueError('Morph targets must be baked')
            for primitive in items: _extensions(primitive)
        buffers = self.doc.get('buffers', [])
        if len(buffers) != 1 or 'uri' in buffers[0]: raise ValueError('Only one embedded geometry buffer is supported')
        size = _int(buffers[0].get('byteLength'), 'buffer length')
        if not size <= len(chunks[1][1]) <= size+3: raise ValueError('Invalid buffer byteLength')
        self.binary = chunks[1][1][:size]
        self._images = {}; self._image_pixels = 0

    def view(self, index):
        definition = _item(self.doc.get('bufferViews'), index, 'buffer view')
        if _int(definition.get('buffer'), 'buffer index') != 0: raise ValueError('External buffer reference')
        offset = _int(definition.get('byteOffset',0), 'view offset')
        length = _int(definition.get('byteLength'), 'view size')
        if offset+length > len(self.binary): raise ValueError('View exceeds buffer')
        return definition, self.binary[offset:offset+length]

    def accessor(self, index):
        definition = _item(self.doc.get('accessors'), index, 'accessor')
        if 'sparse' in definition: raise ValueError('Sparse accessors must be baked')
        formats = {5120:('i1',1),5121:('u1',1),5122:('<i2',2),5123:('<u2',2),5125:('<u4',4),5126:('<f4',4)}
        component = definition.get('componentType'); shape = definition.get('type')
        if type(component) is not int or component not in formats or not isinstance(shape,str) or shape not in {'SCALAR','VEC2','VEC3','VEC4'}: raise ValueError('Unsupported accessor format')
        dtype, width = formats[component]; dimensions = 1 if shape == 'SCALAR' else int(shape[-1])
        count = _int(definition.get('count'), 'accessor count', MAX_VERTICES)
        if not count: raise ValueError('Empty accessor')
        view, data = self.view(definition.get('bufferView'))
        stride = _int(view.get('byteStride', dimensions*width), 'stride', 252)
        offset = _int(definition.get('byteOffset',0), 'accessor offset')
        if ('byteStride' in view and (stride < 4 or stride % 4)) or stride < dimensions*width or stride % width or offset % width or (view.get('byteOffset',0)+offset) % width or offset+(count-1)*stride+dimensions*width > len(data):
            raise ValueError('Accessor exceeds or misaligns its buffer view')
        normalized = definition.get('normalized', False)
        if not isinstance(normalized,bool) or (normalized and component not in (5120,5121,5122,5123)):
            raise ValueError('Invalid accessor normalization')
        values = np.ndarray((count, dimensions), dtype=dtype, buffer=data, offset=offset, strides=(stride,width))
        if not np.isfinite(values).all(): raise ValueError('Non-finite accessor')
        if normalized:
            values = np.maximum(-1, values.astype(np.float32)/np.float32({5120:127,5121:255,5122:32767,5123:65535}[component]))
        return values

    def image(self, index):
        if index in self._images: return self._images[index]
        definition = _item(self.doc.get('images'), index, 'image')
        if ('bufferView' in definition) == ('uri' in definition): raise ValueError('Image requires one embedded source')
        if 'bufferView' in definition:
            if definition.get('mimeType') not in ('image/png','image/jpeg'): raise ValueError('Unsupported image MIME')
            data = self.view(definition['bufferView'])[1]
        else:
            uri = definition['uri']
            if not isinstance(uri,str) or not uri.startswith(('data:image/png;base64,','data:image/jpeg;base64,')) or len(uri) > MAX_IMAGE_BYTES*4//3+64:
                raise ValueError('Image must be a bounded embedded PNG or JPEG')
            data = base64.b64decode(uri.split(',',1)[1], validate=True)
        if not data or len(data) > MAX_IMAGE_BYTES: raise ValueError('Image byte limit exceeded')
        try:
            with Image.open(io.BytesIO(data)) as source:
                width, height = source.size; pixels = width*height
                if source.format not in ('PNG','JPEG') or not 0 < width <= MAX_IMAGE_DIMENSION or not 0 < height <= MAX_IMAGE_DIMENSION or pixels > MAX_IMAGE_PIXELS or self._image_pixels+pixels > MAX_TOTAL_IMAGE_PIXELS:
                    raise ValueError('Image decoded pixel limit exceeded')
                if getattr(source,'n_frames',1) != 1: raise ValueError('Animated images must be baked')
                expected_mime = 'image/png' if source.format == 'PNG' else 'image/jpeg'
                if definition.get('mimeType',expected_mime) != expected_mime: raise ValueError('Image MIME does not match bytes')
                if 'uri' in definition and not definition['uri'].startswith('data:'+expected_mime+';base64,'):
                    raise ValueError('Image MIME does not match bytes')
                self._images[index] = source.convert('RGB')
                self._image_pixels += pixels
        except (OSError,Image.DecompressionBombError) as error:
            raise ValueError('Invalid embedded image') from error
        return self._images[index]

class Material(NamedTuple):
    factor: np.ndarray
    texture: object
    sampler: dict
    transform: dict
    double_sided: bool = False


def load(path):
    """Return actual render geometry; host face/corner proof is still mandatory."""
    glb = _SemanticGlb(path)
    vertices, faces, uv, colors, materials, material_ids = [], [], [], [], [], []
    offset = face_count = 0
    texture_cache = {}
    for primitive, matrix in primitives(glb):
        if not isinstance(primitive, dict) or type(primitive.get('mode',4)) is not int or primitive.get('mode', 4) != 4:
            raise ValueError('Static triangles only')
        attr = primitive.get('attributes')
        if not isinstance(attr, dict) or 'POSITION' not in attr:
            raise ValueError('Missing POSITION')
        position = _item(glb.doc.get('accessors'), attr['POSITION'], 'POSITION accessor')
        count = position.get('count')
        if isinstance(count, bool) or not isinstance(count, int) or count <= 0 or offset + count > MAX_VERTICES:
            raise ValueError('Invalid or excessive vertex count')
        if position.get('type') != 'VEC3' or position.get('componentType') != 5126 or position.get('normalized',False) is not False:
            raise ValueError('Invalid POSITION format')
        for semantic, reference in attr.items():
            if semantic not in ('POSITION','NORMAL','TANGENT','TEXCOORD_0','COLOR_0'):
                raise ValueError('Unsupported static vertex attribute')
            accessor = _item(glb.doc.get('accessors'), reference, 'attribute accessor')
            if isinstance(accessor.get('count'), bool) or accessor.get('count') != count:
                raise ValueError('Primitive attribute counts differ')
            if not isinstance(accessor.get('normalized', False), bool):
                raise ValueError('Invalid attribute normalized flag')
            expected = ('VEC2',) if semantic.startswith('TEXCOORD_') else ('VEC3', 'VEC4') if semantic.startswith('COLOR_') else ('VEC3',) if semantic in ('POSITION', 'NORMAL') else ('VEC4',) if semantic == 'TANGENT' or semantic.startswith(('JOINTS_', 'WEIGHTS_')) else ()
            if expected and accessor.get('type') not in expected:
                raise ValueError('Invalid attribute shape: ' + semantic)
            if semantic.startswith(('COLOR_', 'TEXCOORD_')):
                component = accessor.get('componentType')
                if component != 5126 and not (component in (5121, 5123) and accessor.get('normalized') is True):
                    raise ValueError('Color/UV attributes require float or normalized unsigned components')
            elif accessor.get('componentType') != 5126 or accessor.get('normalized',False) is not False:
                raise ValueError('Geometry attributes must be non-normalized float32')
            glb.accessor(reference)  # Validate even attributes unused by base-color shading.
        if 'indices' in primitive:
            accessor = _item(glb.doc.get('accessors'), primitive['indices'], 'index accessor')
            if accessor.get('type') != 'SCALAR' or accessor.get('componentType') not in (5121, 5123, 5125) or accessor.get('normalized', False) is not False:
                raise ValueError('Indices must be unsigned non-normalized scalars')
            index_count = accessor.get('count')
            index_view = _item(glb.doc.get('bufferViews'),accessor.get('bufferView'),'index view')
            if 'byteStride' in index_view: raise ValueError('Interleaved index accessors are unsupported')
        else:
            index_count = count
        if isinstance(index_count, bool) or not isinstance(index_count, int) or index_count <= 0 or index_count % 3 or face_count + index_count // 3 > MAX_FACES:
            raise ValueError('Invalid or excessive triangle count')
        indices = np.asarray(glb.accessor(primitive['indices']), dtype=np.int64).reshape(-1) if 'indices' in primitive else np.arange(count, dtype=np.int64)
        if np.any(indices < 0) or np.any(indices >= count):
            raise ValueError('Index outside its primitive POSITION array')
        idx = indices.reshape(-1, 3).astype(np.int32)
        pos, idx = apply_transform(glb.accessor(attr['POSITION']), idx, matrix)
        if not np.isfinite(pos).all(): raise ValueError('Non-finite transformed geometry')
        vertices.append(pos)
        faces.append(idx + offset)
        color = np.asarray(glb.accessor(attr['COLOR_0']), dtype=np.float64) if 'COLOR_0' in attr else np.ones((count, 3))
        if np.any(color < 0) or np.any(color > 1): raise ValueError('Vertex colors outside [0,1]')
        colors.append(color[:, :3])
        uv.append(np.asarray(glb.accessor(attr['TEXCOORD_0']), dtype=np.float64) if 'TEXCOORD_0' in attr else np.zeros((count, 2)))
        material = _item(glb.doc.get('materials', []), primitive['material'], 'material') if 'material' in primitive else {}
        extensions = _extensions(material, ('KHR_materials_unlit',))
        if extensions.get('KHR_materials_unlit', {}) != {}: raise ValueError('Invalid unlit extension')
        if material.get('alphaMode', 'OPAQUE') != 'OPAQUE':
            raise ValueError('Only OPAQUE materials are supported; alpha coverage is not baked')
        double_sided = material.get('doubleSided', False)
        if not isinstance(double_sided, bool): raise ValueError('Invalid doubleSided')
        pbr = material.get('pbrMetallicRoughness', {})
        if not isinstance(pbr, dict): raise ValueError('Invalid material PBR object')
        _extensions(pbr)
        factor = np.asarray(_numbers(pbr.get('baseColorFactor', [1, 1, 1, 1]), 4, 'base color'))
        if np.any(factor < 0) or np.any(factor > 1): raise ValueError('Base color outside [0,1]')
        ti = pbr.get('baseColorTexture')
        tex, sampler, tex_transform = None, {}, {}
        if ti is not None:
            if not isinstance(ti, dict): raise ValueError('Invalid base color texture')
            tex_transform = _extensions(ti, ('KHR_texture_transform',)).get('KHR_texture_transform', {})
            if not isinstance(tex_transform,dict) or set(tex_transform)-{'offset','rotation','scale','texCoord'}: raise ValueError('Invalid texture transform')
            tex_coord = tex_transform.get('texCoord', ti.get('texCoord', 0))
            if type(tex_coord) is not int or tex_coord != 0 or 'TEXCOORD_0' not in attr: raise ValueError('Missing supported UV')
            definition = _item(glb.doc.get('textures', []), ti.get('index'), 'texture')
            source = definition.get('source')
            _item(glb.doc.get('images', []), source, 'texture source')
            if source not in texture_cache:
                encoded = np.asarray(glb.image(source), dtype=np.float32) / 255
                texture_cache[source] = np.where(encoded <= .04045, encoded/12.92, ((encoded+.055)/1.055)**2.4)
            tex = texture_cache[source]
            sampler = _item(glb.doc.get('samplers', []), definition['sampler'], 'sampler') if 'sampler' in definition else {}
            for key in ('wrapS', 'wrapT'):
                if sampler.get(key, 10497) not in (10497, 33071, 33648): raise ValueError('Invalid wrapping')
            if sampler.get('magFilter', 9729) not in (9728, 9729) or sampler.get('minFilter', 9729) not in (9728, 9729, 9984, 9985, 9986, 9987):
                raise ValueError('Invalid filtering')
            _numbers(tex_transform.get('scale', [1, 1]), 2, 'texture scale')
            _numbers(tex_transform.get('offset', [0, 0]), 2, 'texture offset')
            _numbers([tex_transform.get('rotation', 0)], 1, 'texture rotation')
        materials.append(Material(factor[:3], tex, sampler, tex_transform, double_sided))
        material_ids.append(np.full(len(idx), len(materials)-1, dtype=np.int32))
        offset += count
        face_count += len(idx)
    if not faces: raise ValueError('No static triangle geometry')
    vertices, faces = np.concatenate(vertices), np.concatenate(faces)
    faces = finalize_winding(vertices, faces)
    return vertices, faces, np.concatenate(uv), np.concatenate(colors), materials, np.concatenate(material_ids)


def project(vertices, basis, center, half_height, size):
    basis = np.asarray(basis, dtype=np.float64)
    center = np.asarray(center, dtype=np.float64)
    vertices = np.asarray(vertices, dtype=np.float64)
    if (basis.shape != (3, 3) or center.shape != (3,) or vertices.ndim != 2 or vertices.shape[1] != 3 or
        not np.isfinite(vertices).all() or not np.isfinite(basis).all() or not np.isfinite(center).all() or
        not np.allclose(basis @ basis.T, np.eye(3), atol=1e-10, rtol=0) or np.linalg.det(basis) < 0 or
        not math.isfinite(half_height) or half_height <= 0 or isinstance(size, bool) or not isinstance(size, int) or not 1 <= size <= 4096):
        raise ValueError('Invalid orthographic camera or vertices')
    camera = (vertices - center) @ basis.T
    with np.errstate(over='ignore', invalid='ignore'):
        projected = np.column_stack(((camera[:, 0] / half_height + 1) * size / 2,
                                     (1-camera[:, 1] / half_height) * size / 2, camera[:, 2]))
    if not np.isfinite(projected).all(): raise ValueError('Non-finite projection')
    return projected


def double_sided_faces(materials, material_ids):
    return np.asarray([m.double_sided for m in materials], dtype=bool)[material_ids]


def raster(vertices, faces, basis, center, half_height, size, double_sided=None):
    projected = project(vertices, basis, center, half_height, size)
    faces = np.asarray(faces)
    if faces.ndim != 2 or faces.shape[1] != 3 or not np.issubdtype(faces.dtype, np.integer) or len(faces) > MAX_FACES or np.any(faces < 0) or np.any(faces >= len(vertices)):
        raise ValueError('Invalid triangle indices')
    sided = np.zeros(len(faces), dtype=bool) if double_sided is None else np.asarray(double_sided)
    if sided.shape != (len(faces),) or sided.dtype != np.bool_: raise ValueError('Invalid per-face doubleSided flags')
    accelerator = _native_raster_kernel()
    if accelerator is not None:
        triangles = np.ascontiguousarray(projected[faces], dtype=np.float64)
        sided_bytes = np.ascontiguousarray(sided, dtype=np.uint8)
        ids = np.empty((size, size), dtype=np.int32)
        depths = np.empty((size, size), dtype=np.float64)
        bary = np.empty((size, size, 3), dtype=np.float64)
        result = accelerator(triangles.ctypes.data, sided_bytes.ctypes.data, len(faces), size,
                             ids.ctypes.data, depths.ctypes.data, bary.ctypes.data)
        if result != 0: raise ValueError('Semantic raster accelerator rejected the view')
        return ids, depths, bary
    triangles = projected[faces]
    # Clip in float before integer conversion, including triangles far offscreen.
    lower = np.clip(np.ceil(triangles[:, :, :2].min(1)-.5), 0, size).astype(int)
    upper = np.clip(np.floor(triangles[:, :, :2].max(1)-.5), -1, size-1).astype(int)
    ids = np.full((size, size), -1, dtype=np.int32)
    depths = np.full((size, size), -np.inf, dtype=np.float64)
    bary = np.zeros((size, size, 3), dtype=np.float64)
    for face_id in np.flatnonzero(np.all(lower <= upper, axis=1)):
        a, b, c = triangles[face_id]
        denominator = (b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
        # Camera looks from +Z; projection flips Y, so front faces have negative area.
        if denominator == 0 or (denominator > 0 and not sided[face_id]): continue
        x0, y0 = lower[face_id]; x1, y1 = upper[face_id]
        yy, xx = np.mgrid[y0:y1+1, x0:x1+1]
        xx = xx + .5; yy = yy + .5
        edges = [((q[1]-r[1])*(xx-r[0])+(r[0]-q[0])*(yy-r[1])) for q, r in ((b,c),(c,a),(a,b))]
        inside = np.ones(xx.shape, dtype=bool)
        sign = 1 if denominator > 0 else -1
        for edge, (q, r) in zip(edges, ((b,c),(c,a),(a,b))):
            dx, dy = (r[:2]-q[:2])*sign
            top_left = dy < 0 or (dy == 0 and dx > 0)
            inside &= (edge*sign > 0) | ((edge == 0) & top_left)
        w0, w1 = edges[0]/denominator, edges[1]/denominator
        w2 = 1-w0-w1
        z = w0*a[2]+w1*b[2]+w2*c[2]
        # Ascending traversal ID + strict float64 comparison: exact ties retain first ID.
        take = inside & (z > depths[y0:y1+1, x0:x1+1])
        depths[y0:y1+1, x0:x1+1][take] = z[take]
        ids[y0:y1+1, x0:x1+1][take] = face_id
        bary[y0:y1+1, x0:x1+1][take] = np.stack((w0, w1, w2), -1)[take]
    return ids, depths, bary


def _transform_uv(coords, transform):
    coords = np.asarray(coords, dtype=np.float64)
    if coords.ndim < 2 or coords.shape[-1] != 2 or not np.isfinite(coords).all():
        raise ValueError('Invalid UV coordinates')
    with np.errstate(over='ignore', invalid='ignore'):
        local = coords * transform.get('scale', [1, 1])
    angle = transform.get('rotation', 0)
    rotation = np.array([[math.cos(angle), -math.sin(angle)], [math.sin(angle), math.cos(angle)]])
    with np.errstate(over='ignore', invalid='ignore'):
        result = local @ rotation.T + transform.get('offset', [0, 0])
    if not np.isfinite(result).all(): raise ValueError('Non-finite transformed UV')
    return result


def _wrap_indices(indices, length, mode):
    if mode == 33071: return np.clip(indices, 0, length-1)
    if mode == 10497: return indices % length
    if mode == 33648:
        indices = indices % (2*length)
        return np.where(indices < length, indices, 2*length-1-indices)
    raise ValueError('Invalid sampler')


def sample_texture(texture, coords, sampler, linear_filter):
    texture = np.asarray(texture)
    if texture.ndim != 3 or texture.shape[2] != 3 or min(texture.shape[:2]) <= 0:
        raise ValueError('Invalid RGB texture shape')
    height, width = texture.shape[:2]
    coords = np.asarray(coords, dtype=np.float64).copy()
    if coords.ndim != 2 or coords.shape[1] != 2 or not np.isfinite(coords).all():
        raise ValueError('Invalid texture sampling coordinates')
    for axis, key in enumerate(('wrapS', 'wrapT')):
        mode = sampler.get(key, 10497)
        if mode == 33071: coords[:, axis] = np.clip(coords[:, axis], 0, 1)
        elif mode == 10497: coords[:, axis] %= 1
        elif mode == 33648: coords[:, axis] %= 2
        else: raise ValueError('Invalid sampler')
    def at(x,y):
        return texture[_wrap_indices(y,height,sampler.get('wrapT',10497)),
                       _wrap_indices(x,width,sampler.get('wrapS',10497))]
    texel = coords * [width,height]
    nearest = np.floor(texel).astype(np.int64)
    result = at(nearest[:,0],nearest[:,1]).copy()
    take = np.broadcast_to(linear_filter, (len(coords),))
    if take.any():
        position = texel[take]-.5
        lo = np.floor(position).astype(np.int64); fraction = position-lo
        x,y = lo.T; fx,fy = fraction.T
        result[take] = ((at(x,y)*(1-fx[:,None])+at(x+1,y)*fx[:,None])*(1-fy[:,None]) +
                        (at(x,y+1)*(1-fx[:,None])+at(x+1,y+1)*fx[:,None])*fy[:,None])
    return result


def shade(faces, uv, colors, materials, material_ids, ids, bary, projected=None):
    rgb = np.full((*ids.shape, 3), 184, dtype=np.uint8)
    valid = ids >= 0; visible = ids[valid]; indices = faces[visible]; weights = bary[valid]
    texcoords = np.sum(uv[indices] * weights[:, :, None], axis=1)
    linear = np.sum(colors[indices] * weights[:, :, None], axis=1)
    for material_id, material in enumerate(materials):
        factor, texture, sampler, transform, _ = material
        take = material_ids[visible] == material_id
        if not take.any(): continue
        if texture is not None:
            local = _transform_uv(texcoords[take], transform)
            mag_linear = sampler.get('magFilter',9729) == 9729
            min_linear = sampler.get('minFilter',9729) in (9729,9985,9987)
            filtering = np.full(int(take.sum()), mag_linear, dtype=bool)
            if projected is None:
                if mag_linear != min_linear: raise ValueError('Projected vertices required to choose mag/min filtering')
            else:
                # Constant derivatives for orthographic interpolation; choose base-level min fallback.
                unique, inverse = np.unique(visible[take], return_inverse=True)
                xy = projected[faces[unique], :2]
                texuv = _transform_uv(uv[faces[unique]], transform) * [texture.shape[1],texture.shape[0]]
                screen_edges = xy[:,1:]-xy[:,:1]
                uv_edges = texuv[:,1:]-texuv[:,:1]
                derivatives = np.linalg.solve(screen_edges, uv_edges)
                footprint = np.max(np.linalg.norm(derivatives,axis=2),axis=1)
                filtering[footprint[inverse] > 1] = min_linear
            linear[take] *= sample_texture(texture,local,sampler,filtering)
        linear[take] *= factor
    linear = np.clip(linear, 0, 1)
    srgb = np.where(linear <= .0031308, 12.92*linear, 1.055*linear**(1/2.4)-.055)
    rgb[valid] = np.rint(srgb*255).astype(np.uint8)
    return rgb

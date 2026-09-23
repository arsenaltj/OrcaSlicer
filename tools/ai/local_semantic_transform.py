"""Restricted native-compatible static transform arithmetic, without normalization.

Expressions follow Assimp 5.4.3 GetNodeTransform, ComputeAbsoluteTransform,
ApplyTransform, vector3.inl and matrix4x4.inl, followed by ModelArtifact's
float32 metre-to-millimetre/Z-up conversion. Other importer versions, compiler
arithmetic and primitive ordering still require the host's per-asset proof.
"""
import numpy as np
from glb_artifact import _item, _numbers

MAX_NODES = 4096
MAX_DEPTH = 64
MAX_MESH_INSTANCES = 64
MAX_PRIMITIVES = 1024
IDENTITY_EPSILON = np.float32(.01)  # Installed Assimp config.h: 10e-3f.


def _finite32(values):
    with np.errstate(over='ignore', invalid='ignore'):
        result = np.asarray(values, dtype=np.float32)
    if not np.isfinite(result).all():
        raise ValueError('Transform exceeds finite native float32 range')
    return result


def multiply(left, right):
    a, b = _finite32(left), _finite32(right)
    if a.shape != (4, 4) or b.shape != (4, 4):
        raise ValueError('Invalid transform matrix shape')
    # Explicit left-associated operations; do not substitute a BLAS reduction.
    with np.errstate(over='ignore', invalid='ignore'):
        result = ((b[None,0,:]*a[:,0,None]+b[None,1,:]*a[:,1,None])+
                  b[None,2,:]*a[:,2,None])+b[None,3,:]*a[:,3,None]
    return _finite32(result)


def node_matrix(node):
    if not isinstance(node, dict):
        raise ValueError('Invalid transform node')
    if 'matrix' in node:
        if any(key in node for key in ('translation', 'rotation', 'scale')):
            raise ValueError('Node cannot combine matrix and TRS')
        values = np.asarray(_numbers(node['matrix'], 16, 'node matrix')).reshape(4, 4).T
        if not np.array_equal(values[3], [0, 0, 0, 1]):
            raise ValueError('Only exact affine node matrices are supported')
        return _finite32(values)
    result = np.eye(4, dtype=np.float32)
    if 'translation' in node:
        t = np.eye(4, dtype=np.float32)
        t[:3, 3] = _finite32(_numbers(node['translation'], 3, 'translation'))
        result = multiply(result, t)
    if 'rotation' in node:
        values = _numbers(node['rotation'], 4, 'rotation')
        # glTF requires unit quaternions. Assimp does not normalize here; reject
        # non-unit input instead of silently changing geometry as the prototype did.
        if abs(sum(v*v for v in values)-1) > 1e-6:
            raise ValueError('Only unit quaternions are supported; bake other rotations')
        x, y, z, w = _finite32(values)
        one, two = np.float32(1), np.float32(2)
        r = np.eye(4, dtype=np.float32)
        r[:3,:3] = [[one-two*(y*y+z*z), two*(x*y-z*w), two*(x*z+y*w)],
                    [two*(x*y+z*w), one-two*(x*x+z*z), two*(y*z-x*w)],
                    [two*(x*z-y*w), two*(y*z+x*w), one-two*(x*x+y*y)]]
        result = multiply(result, r)
    if 'scale' in node:
        s = np.eye(4, dtype=np.float32)
        s[range(3), range(3)] = _finite32(_numbers(node['scale'], 3, 'scale'))
        result = multiply(result, s)
    return result


def is_identity(matrix):
    # Same float32 endpoints as aiMatrix4x4t::IsIdentity; this intentional native
    # compatibility rule must not be mistaken for an ideal glTF transform.
    identity = np.eye(4, dtype=np.float32)
    return bool(np.all(matrix <= identity+IDENTITY_EPSILON) and np.all(matrix >= identity-IDENTITY_EPSILON))


def determinant(matrix):
    (a1,a2,a3,a4),(b1,b2,b3,b4),(c1,c2,c3,c4),(d1,d2,d3,d4) = matrix
    with np.errstate(over='ignore', invalid='ignore'):
        value = (a1*b2*c3*d4-a1*b2*c4*d3+a1*b3*c4*d2-a1*b3*c2*d4
                 +a1*b4*c2*d3-a1*b4*c3*d2-a2*b3*c4*d1+a2*b3*c1*d4
                 -a2*b4*c1*d3+a2*b4*c3*d1-a2*b1*c3*d4+a2*b1*c4*d3
                 +a3*b4*c1*d2-a3*b4*c2*d1+a3*b1*c2*d4-a3*b1*c4*d2
                 +a3*b2*c4*d1-a3*b2*c1*d4-a4*b1*c2*d3+a4*b1*c3*d2
                 -a4*b2*c3*d1+a4*b2*c1*d3-a4*b3*c1*d2+a4*b3*c2*d1)
    if not np.isfinite(value) or value == 0:
        raise ValueError('Singular or unstable float32 transform determinant')
    # Reject a sign disagreement rather than guessing a winding convention.
    double = np.linalg.det(matrix.astype(np.float64))
    if not np.isfinite(double) or double == 0 or np.signbit(value) != np.signbit(double):
        raise ValueError('Unstable float32 transform determinant sign')
    return value


def apply_transform(positions, indices, matrix):
    p, m = _finite32(positions), _finite32(matrix)
    if p.ndim != 2 or p.shape[1] != 3 or m.shape != (4, 4):
        raise ValueError('Invalid native transform input')
    faces = np.asarray(indices, dtype=np.int32).copy()
    if not is_identity(m):
        if determinant(m) < 0:
            faces = faces[:, ::-1].copy()  # Assimp reverses all corners, not just 1/2.
        with np.errstate(over='ignore', invalid='ignore'):
            p = np.stack([((m[r,0]*p[:,0]+m[r,1]*p[:,1])+m[r,2]*p[:,2])+m[r,3] for r in range(3)], axis=1)
    with np.errstate(over='ignore', invalid='ignore'):
        p = p[:, [0,2,1]] * np.array([1000,-1000,1000], dtype=np.float32)
    return _finite32(p), faces


def native_signed_volume(vertices, faces):
    """ModelArtifact's final float32 its_volume calculation, after concatenation.

    This is the importer's orientation statistic, not physical volume for open
    surfaces. Chunked cumsum preserves ordered float32 addition (no pairwise sum).
    Host proof remains necessary if Assimp vertex reordering changes the first
    reference vertex, or a different Eigen/compiler changes floating arithmetic.
    """
    vertices = _finite32(vertices)
    faces = np.asarray(faces)
    if vertices.ndim != 2 or vertices.shape[1] != 3 or not len(vertices) or faces.ndim != 2 or faces.shape[1] != 3 or not len(faces) or faces.dtype.kind not in 'iu' or faces.min() < 0 or faces.max() >= len(vertices):
        raise ValueError('Invalid mesh for native orientation statistic')
    reference = vertices[0]
    volume, absolute_terms = np.float32(0), 0.
    for start in range(0, len(faces), 4096):
        triangle = vertices[faces[start:start+4096]]
        with np.errstate(over='ignore', invalid='ignore', divide='ignore'):
            u, v = triangle[:,1]-triangle[:,0], triangle[:,2]-triangle[:,0]
            cross = np.cross(u, v)
            square = cross*cross
            # Current Eigen fixed-size vector norm redux groups 0+(1+2).
            norm = np.sqrt(square[:,0]+(square[:,1]+square[:,2]))
            if not np.isfinite(norm).all() or np.any(norm <= 0):
                raise ValueError('Degenerate or non-finite native orientation statistic')
            normal = cross/norm[:,None]
            product = normal*(triangle[:,0]-reference)
            # Current Eigen 3-component dot accumulates in coefficient order.
            height = (product[:,0]+product[:,1])+product[:,2]
            terms = ((np.float32(.5)*norm)*height)/np.float32(3)
            volume = np.cumsum(np.concatenate((np.array([volume],np.float32),terms)),dtype=np.float32)[-1]
        if not np.isfinite(terms).all() or not np.isfinite(volume):
            raise ValueError('Non-finite native orientation statistic')
        absolute_terms += float(np.sum(np.abs(terms.astype(np.float64))))
    # Do not choose a side when opposing contributions almost cancel. Exact
    # planar zero (all terms zero) remains valid. This is a conservative guard,
    # not a claim of equivalence to every compiler or importer configuration.
    if absolute_terms and abs(float(volume)) <= 32*np.finfo(np.float32).eps*absolute_terms:
        raise ValueError('Unstable native orientation statistic')
    return volume


def finalize_winding(vertices, faces):
    """Mirror ModelArtifact's final mesh.volume()<0 / flip_triangles step."""
    if native_signed_volume(vertices, faces) < 0:
        return np.asarray(faces)[:,[0,2,1]].copy()
    return faces


def primitives(glb):
    """Visit a bounded selected scene; repeated mesh instances are counted.

    A node cannot have multiple parents/occur twice. Mesh instancing via distinct
    nodes is bounded but does not promise Assimp mesh/vertex enumeration order.
    """
    doc = glb.doc
    if len(doc.get('nodes', [])) > MAX_NODES or not doc.get('scenes'):
        raise ValueError('A bounded explicit scene is required')
    roots = _item(doc['scenes'], doc.get('scene', 0), 'scene').get('nodes', [])
    if not isinstance(roots, list):
        raise ValueError('Invalid scene roots')
    visited, instances, emitted = set(), 0, 0

    def visit(index, parent, depth):
        nonlocal instances, emitted
        node = _item(doc.get('nodes'), index, 'node')
        if index in visited or len(visited) >= MAX_NODES or depth > MAX_DEPTH:
            raise ValueError('Repeated node, cycle or excessive scene depth')
        visited.add(index)
        if 'skin' in node or 'weights' in node:
            raise ValueError('Skins and node weights must be baked')
        world = multiply(parent, node_matrix(node))
        if 'mesh' in node:
            instances += 1
            if instances > MAX_MESH_INSTANCES:
                raise ValueError('Too many mesh instances')
            mesh = _item(doc.get('meshes'), node['mesh'], 'mesh')
            for primitive in mesh.get('primitives', []):
                emitted += 1
                if emitted > MAX_PRIMITIVES:
                    raise ValueError('Too many primitive instances')
                yield primitive, world
        children = node.get('children', [])
        if not isinstance(children, list):
            raise ValueError('Invalid child nodes')
        for child in children:
            yield from visit(child, world, depth+1)

    for root in roots:
        yield from visit(root, np.eye(4, dtype=np.float32), 0)

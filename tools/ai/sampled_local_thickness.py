"""Bounded, dependency-free local surface-thickness sampling for generated meshes."""

from __future__ import annotations

from array import array
import heapq
import math
from typing import Sequence


Vec3 = tuple[float, float, float]
Face = tuple[int, int, int]
Node = tuple[float, float, float, float, float, float, int, int, int, int]


def _spread_morton_bits(value: int) -> int:
    value &= 0x3FF
    value = (value | value << 16) & 0x030000FF
    value = (value | value << 8) & 0x0300F00F
    value = (value | value << 4) & 0x030C30C3
    return (value | value << 2) & 0x09249249


def _morton_code(x: int, y: int, z: int) -> int:
    return _spread_morton_bits(x) | (_spread_morton_bits(y) << 1) | (_spread_morton_bits(z) << 2)


def _ray_hits_box(origin: Vec3, direction: Vec3, node: Node, maximum_distance: float) -> bool:
    near_distance = 0.0
    far_distance = maximum_distance
    for axis in range(3):
        minimum = node[axis]
        maximum = node[axis + 3]
        component = direction[axis]
        if abs(component) <= 1e-12:
            if origin[axis] < minimum - 1e-9 or origin[axis] > maximum + 1e-9:
                return False
            continue
        first = (minimum - origin[axis]) / component
        second = (maximum - origin[axis]) / component
        if first > second:
            first, second = second, first
        near_distance = max(near_distance, first)
        far_distance = min(far_distance, second)
        if near_distance > far_distance:
            return False
    return far_distance > 0.0


def _ray_triangle_distance(origin: Vec3, direction: Vec3, a: Vec3, b: Vec3, c: Vec3) -> float | None:
    edge_a = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    edge_b = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
    cross = (
        direction[1] * edge_b[2] - direction[2] * edge_b[1],
        direction[2] * edge_b[0] - direction[0] * edge_b[2],
        direction[0] * edge_b[1] - direction[1] * edge_b[0],
    )
    determinant = edge_a[0] * cross[0] + edge_a[1] * cross[1] + edge_a[2] * cross[2]
    if abs(determinant) <= 1e-12:
        return None
    inverse = 1.0 / determinant
    offset = (origin[0] - a[0], origin[1] - a[1], origin[2] - a[2])
    u = (offset[0] * cross[0] + offset[1] * cross[1] + offset[2] * cross[2]) * inverse
    if u < 0.0 or u > 1.0:
        return None
    q = (
        offset[1] * edge_a[2] - offset[2] * edge_a[1],
        offset[2] * edge_a[0] - offset[0] * edge_a[2],
        offset[0] * edge_a[1] - offset[1] * edge_a[0],
    )
    v = (direction[0] * q[0] + direction[1] * q[1] + direction[2] * q[2]) * inverse
    if v < 0.0 or u + v > 1.0:
        return None
    distance = (edge_b[0] * q[0] + edge_b[1] * q[1] + edge_b[2] * q[2]) * inverse
    return distance if distance > 1e-9 else None


def _build_bvh(vertices: Sequence[Vec3], faces: Sequence[Face], leaf_size: int) -> tuple[list[int], list[Node]]:
    face_bounds = array("d")
    minimum = [math.inf, math.inf, math.inf]
    maximum = [-math.inf, -math.inf, -math.inf]
    for face in faces:
        points = (vertices[face[0]], vertices[face[1]], vertices[face[2]])
        bounds = (
            min(point[0] for point in points),
            min(point[1] for point in points),
            min(point[2] for point in points),
            max(point[0] for point in points),
            max(point[1] for point in points),
            max(point[2] for point in points),
        )
        face_bounds.extend(bounds)
        for axis in range(3):
            minimum[axis] = min(minimum[axis], bounds[axis])
            maximum[axis] = max(maximum[axis], bounds[axis + 3])

    scales = [1023.0 / (maximum[axis] - minimum[axis]) if maximum[axis] > minimum[axis] else 0.0 for axis in range(3)]
    codes: list[int] = []
    for face_index in range(len(faces)):
        offset = face_index * 6
        center = tuple((face_bounds[offset + axis] + face_bounds[offset + axis + 3]) * 0.5 for axis in range(3))
        quantized = tuple(
            max(0, min(1023, int((center[axis] - minimum[axis]) * scales[axis] + 0.5)))
            for axis in range(3)
        )
        codes.append(_morton_code(*quantized))
    order = list(range(len(faces)))
    order.sort(key=codes.__getitem__)
    del codes

    nodes: list[Node] = []

    def build(begin: int, end: int) -> int:
        node_index = len(nodes)
        nodes.append((0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -1, -1, 0, 0))
        count = end - begin
        if count <= leaf_size:
            bounds = [math.inf, math.inf, math.inf, -math.inf, -math.inf, -math.inf]
            for item in range(begin, end):
                offset = order[item] * 6
                for axis in range(3):
                    bounds[axis] = min(bounds[axis], face_bounds[offset + axis])
                    bounds[axis + 3] = max(bounds[axis + 3], face_bounds[offset + axis + 3])
            nodes[node_index] = (*bounds, -1, -1, begin, count)
            return node_index
        middle = begin + count // 2
        left = build(begin, middle)
        right = build(middle, end)
        left_node, right_node = nodes[left], nodes[right]
        bounds = (
            min(left_node[0], right_node[0]), min(left_node[1], right_node[1]), min(left_node[2], right_node[2]),
            max(left_node[3], right_node[3]), max(left_node[4], right_node[4]), max(left_node[5], right_node[5]),
        )
        nodes[node_index] = (*bounds, left, right, 0, 0)
        return node_index

    build(0, len(order))
    return order, nodes


def sample_local_thickness(
    vertices: Sequence[Vec3],
    faces: Sequence[Face],
    face_areas: Sequence[float],
    face_normals: Sequence[Vec3],
    face_neighbors: Sequence[set[int]],
    face_components: Sequence[int],
    *,
    maximum_distance: float,
    sample_limit: int,
    bvh_leaf_size: int,
    maximum_opposing_normal_dot: float,
) -> tuple[int, int, float, float | None, list[tuple[int, float]]]:
    """Return bounded sampling metrics and measured source-face hits."""

    if not faces or maximum_distance <= 0.0 or sample_limit <= 0:
        return 0, 0, 0.0, None, []
    order, nodes = _build_bvh(vertices, faces, max(4, bvh_leaf_size))
    if len(faces) <= sample_limit:
        samples = list(range(len(faces)))
    else:
        stride = len(order) / sample_limit
        samples = [order[min(len(order) - 1, int((index + 0.5) * stride))] for index in range(sample_limit)]
        largest_limit = min(len(faces), max(64, sample_limit // 8))
        samples.extend(heapq.nlargest(largest_limit, range(len(faces)), key=face_areas.__getitem__))
        samples = list(dict.fromkeys(samples))

    epsilon = max(1e-7, min(1e-4, maximum_distance * 1e-4))
    thin_count = 0
    thin_area = 0.0
    minimum_thickness: float | None = None
    thin_hits: list[tuple[int, float]] = []
    for sample in samples:
        face = faces[sample]
        points = (vertices[face[0]], vertices[face[1]], vertices[face[2]])
        center = tuple(sum(point[axis] for point in points) / 3.0 for axis in range(3))
        normal = face_normals[sample]
        nearest: float | None = None
        excluded = face_neighbors[sample]
        for sign in (-1.0, 1.0):
            direction = (normal[0] * sign, normal[1] * sign, normal[2] * sign)
            origin = tuple(center[axis] + direction[axis] * epsilon for axis in range(3))
            stack = [0]
            while stack:
                node_index = stack.pop()
                node = nodes[node_index]
                if not _ray_hits_box(origin, direction, node, maximum_distance):
                    continue
                if node[9] == 0:
                    stack.append(node[6])
                    stack.append(node[7])
                    continue
                for item in range(node[8], node[8] + node[9]):
                    candidate = order[item]
                    if candidate == sample or candidate in excluded or face_components[candidate] != face_components[sample]:
                        continue
                    candidate_normal = face_normals[candidate]
                    normal_dot = sum(normal[axis] * candidate_normal[axis] for axis in range(3))
                    if normal_dot > maximum_opposing_normal_dot:
                        continue
                    candidate_face = faces[candidate]
                    distance = _ray_triangle_distance(
                        origin,
                        direction,
                        vertices[candidate_face[0]],
                        vertices[candidate_face[1]],
                        vertices[candidate_face[2]],
                    )
                    if distance is None or distance > maximum_distance:
                        continue
                    thickness = distance + epsilon
                    if nearest is None or thickness < nearest:
                        nearest = thickness
        if nearest is None or nearest >= maximum_distance:
            continue
        thin_count += 1
        thin_area += face_areas[sample]
        thin_hits.append((sample, nearest))
        minimum_thickness = nearest if minimum_thickness is None else min(minimum_thickness, nearest)

    return len(samples), thin_count, thin_area, minimum_thickness, thin_hits

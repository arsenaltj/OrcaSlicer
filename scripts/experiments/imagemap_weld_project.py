"""Exact-coordinate topology normalization of an ImageMap 3MF experiment.

Preserves triangle order/corners, independent per-corner UV property indices,
all texture bytes, placement and print configuration. No mesh repair tolerance,
hole fill, decimation or shape edit. Not part of product import code.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import time
import zipfile
import numpy as np


VERTEX=re.compile(rb'<vertex\s+x="([^"]+)"\s+y="([^"]+)"\s+z="([^"]+)"\s*/>')
TRIANGLE=re.compile(rb'<triangle\s+v1="(\d+)"\s+v2="(\d+)"\s+v3="(\d+)"([^>]*)/>')


def weld_xml(xml):
    if xml.count(b'<vertices>') != 1 or xml.count(b'<triangles>') != 1:
        raise ValueError('Exactly one canonical mesh required')
    start=xml.index(b'<vertices>')+len(b'<vertices>');end=xml.index(b'</vertices>',start)
    raw=xml[start:end];matches=VERTEX.findall(raw)
    if not matches or VERTEX.sub(b'',raw).strip():raise ValueError('Unsupported vertex attributes')
    vertices=np.asarray(matches,dtype=np.float64)
    if not np.isfinite(vertices).all():raise ValueError('Non-finite geometry')
    unique,first,inverse=np.unique(vertices,axis=0,return_index=True,return_inverse=True)
    triangle_start=xml.index(b'<triangles>')+len(b'<triangles>')
    triangle_raw=xml[triangle_start:xml.index(b'</triangles>',triangle_start)]
    if TRIANGLE.sub(b'',triangle_raw).strip():
        raise ValueError('Unsupported triangle encoding')
    triangle_matches=TRIANGLE.findall(triangle_raw)
    faces=np.array([t[:3] for t in triangle_matches],dtype=np.int32)
    if faces.size==0 or faces.min()<0 or faces.max()>=len(vertices):raise ValueError('Invalid triangles')
    new_faces=inverse[faces]
    if np.any(new_faces[:,0]==new_faces[:,1]) or np.any(new_faces[:,0]==new_faces[:,2]) or np.any(new_faces[:,1]==new_faces[:,2]):
        raise ValueError('Degenerate face: normalization is not a repair operation')
    assert np.array_equal(unique[new_faces],vertices[faces])
    new_vertices=b'\n'+b'\n'.join(b'     <vertex x="'+matches[i][0]+b'" y="'+matches[i][1]+b'" z="'+matches[i][2]+b'" />' for i in first)+b'\n'
    updated=xml[:start]+new_vertices+xml[end:]
    face_iter=iter(new_faces)
    def replace(match):
        a,b,c=next(face_iter)
        return f'<triangle v1="{a}" v2="{b}" v3="{c}"'.encode()+match[4]+b'/>'
    updated=TRIANGLE.sub(replace,updated)
    if next(face_iter,None) is not None:raise ValueError('Incomplete face replacement')
    # UV and other triangle properties are byte-for-byte identical.
    original_props=[t[3] for t in triangle_matches]
    restored=TRIANGLE.findall(updated)
    assert original_props==[t[3] for t in restored]
    restored_vertices=np.array(VERTEX.findall(updated[start:updated.index(b'</vertices>',start)]),dtype=np.float64)
    restored_faces=np.array([t[:3] for t in restored],dtype=np.int32)
    assert np.array_equal(restored_vertices[restored_faces],vertices[faces])
    # All non-geometry XML including complete texture-coordinate tables stays
    # byte-identical; vertex/triangle sections alone may differ.
    stripped=lambda data:re.sub(rb'<vertices>.*?</vertices>',b'<vertices/>',TRIANGLE.sub(b'<triangle/>',data),flags=re.S)
    assert stripped(xml)==stripped(updated)
    edges=np.sort(new_faces[:,[[0,1],[1,2],[2,0]]].reshape(-1,2),axis=1)
    _,counts=np.unique(edges,axis=0,return_counts=True)
    return updated,{'vertices_before':len(vertices),'vertices_after':len(unique),'faces':len(faces),
                    'unpaired_or_nonmanifold_edges_after':int(np.count_nonzero(counts!=2)),
                    'exact_triangle_corners_roundtrip':True,'triangle_properties_unchanged':True,
                    'other_xml_unchanged':True}


def main():
    parser=argparse.ArgumentParser();parser.add_argument('source',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args()
    if args.output.exists():raise ValueError('Output must be a new experimental project')
    start=time.monotonic();sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    original_sha=sha(args.source)
    with zipfile.ZipFile(args.source) as archive:
        candidates=[n for n in archive.namelist() if n.endswith('.model') and b'<vertices>' in archive.read(n)]
        if len(candidates)!=1:raise ValueError('Exactly one textured mesh required')
        name=candidates[0];updated,checks=weld_xml(archive.read(name))
        with zipfile.ZipFile(args.output,'x',compression=zipfile.ZIP_DEFLATED) as out:
            for info in archive.infolist():
                out.writestr(copy.copy(info),updated if info.filename==name else archive.read(info.filename))
        with zipfile.ZipFile(args.output) as check:
            assert check.read(name)==updated
            for info in archive.infolist():
                if info.filename!=name:assert check.read(info.filename)==archive.read(info.filename)
    assert sha(args.source)==original_sha
    report={'source_sha256':original_sha,'output_sha256':sha(args.output),'implementation_sha256':sha(Path(__file__)),
            'checks':{**checks,'other_project_members_unchanged':True,'source_unchanged':True},
            'seconds':time.monotonic()-start,'native_reopen':'NOT_RUN','slice':'NOT_RUN'}
    args.output.with_suffix('.weld.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report),flush=True)


if __name__=='__main__':main()

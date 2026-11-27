"""Public Slicing C ABI preserves 3MF component world geometry.

Usage: python three_mf_component_world_test.py DLL OUTPUT --fixtures DIRECTORY
The DLL directory must contain its actual runtime dependencies. This is a host
test; it does not establish Android or device execution qualification.
"""
from pathlib import Path
import argparse,ctypes as C,hashlib,json,os,struct,tempfile,uuid,xml.etree.ElementTree as ET,zipfile

NS='{http://schemas.microsoft.com/3dmanufacturing/core/2015/02}'
ET.register_namespace('',NS[1:-1])
IDENTITY=[1,0,0,0,1,0,0,0,1,0,0,0]
VERTICES=[(20,20,0),(30,20,0),(30,30,0),(20,30,0),(20,20,10),(30,20,10),(30,30,10),(20,30,10)]
FACES=[(0,2,1),(0,3,2),(4,5,6),(4,6,7),(0,1,5),(0,5,4),(1,2,6),(1,6,5),(2,3,7),(2,7,6),(3,0,4),(3,4,7)]
ORIGINALS={'single-component-mirror.3mf':'708041353d126da3579912683b0e33588a9a4fbf8e43459f17e42fdce5d4932c','double-component-mirror.3mf':'2341ba2ebf61e279a4c2641a088b5d88f9562ef8441f5d1bf067f2f300d8ce23'}

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def matrix(values):
    a=list(map(float,values.split())) if isinstance(values,str) else values
    return [[a[0],a[3],a[6],a[9]],[a[1],a[4],a[7],a[10]],[a[2],a[5],a[8],a[11]],[0,0,0,1]]
def product(a,b):return [[sum(a[i][k]*b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]
def apply(a,v):return tuple(sum(a[i][k]*v[k] for k in range(3))+a[i][3] for i in range(3))
def determinant(a):
    return a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])-a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])+a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0])

def expected_scene(source):
    with zipfile.ZipFile(source) as archive:root=ET.fromstring(archive.read('3D/3dmodel.model'))
    objects={o.attrib['id']:o for o in root.find(NS+'resources').findall(NS+'object')}
    vertices=[];faces=[]
    def visit(oid,transform,stack=()):
        assert oid not in stack
        obj=objects[oid];mesh=obj.find(NS+'mesh')
        if mesh is not None:
            offset=len(vertices)
            vertices.extend(apply(transform,tuple(float(v.attrib[k]) for k in ('x','y','z'))) for v in mesh.find(NS+'vertices'))
            for t in mesh.find(NS+'triangles'):
                face=tuple(int(t.attrib[k])+offset for k in ('v1','v2','v3'))
                if determinant(transform)<0:face=(face[0],face[2],face[1])
                faces.append(face)
        else:
            for child in obj.find(NS+'components'):
                visit(child.attrib['objectid'],product(transform,matrix(child.attrib.get('transform',IDENTITY))),stack+(oid,))
    for item in root.find(NS+'build'):
        visit(item.attrib['objectid'],matrix(item.attrib.get('transform',IDENTITY)))
    return vertices,faces

def scene(path):
    data=Path(path).read_bytes();assert data[:8]==b'ORCASC01'
    nv,nt,np,flags,reserved=struct.unpack_from('<IIIIQ',data,8);assert flags&1
    vertices=list(struct.iter_unpack('<fff',data[32:32+12*nv]));at=32+12*nv
    faces=list(struct.iter_unpack('<III',data[at:at+12*nt]));at+=12*nt
    materials=list(struct.unpack_from('<'+'H'*nt,data,at));at+=nt*2
    palette=list(struct.iter_unpack('<BBBB',data[at:at+4*np]))
    return dict(vertices=vertices,faces=faces,materials=materials,palette=palette,sha256=sha(path))

def make_source(path,transforms,outer=IDENTITY,nested=False,flat=False,zero_center=False):
    local=[(x-25,y-25,z-5) for x,y,z in VERTICES] if zero_center else VERTICES
    vertices=''.join('<vertex x="%s" y="%s" z="%s"/>'%v for v in local)
    triangles=''.join('<triangle v1="%d" v2="%d" v3="%d"/>'%t for t in FACES)
    model=ET.Element(NS+'model',{'unit':'millimeter'});resources=ET.SubElement(model,NS+'resources')
    obj=ET.SubElement(resources,NS+'object',{'id':'1','type':'model'})
    mesh=ET.SubElement(obj,NS+'mesh');mesh.append(ET.fromstring('<vertices xmlns="'+NS[1:-1]+'">'+vertices+'</vertices>'));mesh.append(ET.fromstring('<triangles xmlns="'+NS[1:-1]+'">'+triangles+'</triangles>'))
    def text(v):return ' '.join(map(str,v))
    build=ET.SubElement(model,NS+'build')
    if flat:
        for transform in transforms:ET.SubElement(build,NS+'item',{'objectid':'1','transform':text(transform)})
    else:
        obj2=ET.SubElement(resources,NS+'object',{'id':'2','type':'model'});parts=ET.SubElement(obj2,NS+'components')
        for transform in transforms:ET.SubElement(parts,NS+'component',{'objectid':'1','transform':text(transform)})
        if nested:
            obj3=ET.SubElement(resources,NS+'object',{'id':'3','type':'model'});parts3=ET.SubElement(obj3,NS+'components')
            ET.SubElement(parts3,NS+'component',{'objectid':'2','transform':text(outer)})
            ET.SubElement(build,NS+'item',{'objectid':'3','transform':text(IDENTITY)})
        else:ET.SubElement(build,NS+'item',{'objectid':'2','transform':text(outer)})
    with zipfile.ZipFile(path,'x') as archive:
        archive.writestr('[Content_Types].xml','<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>')
        archive.writestr('_rels/.rels','<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
        archive.writestr('3D/3dmodel.model',ET.tostring(model,encoding='utf-8',xml_declaration=True))

def main():
    parser=argparse.ArgumentParser();parser.add_argument('library',type=Path);parser.add_argument('output',type=Path,nargs='?')
    parser.add_argument('--fixtures',type=Path,default=Path(__file__).parent/'data/component-world')
    parser.add_argument('--public-operations',action='store_true',help='Also recommend/validate/slice the original bed-valid fixtures')
    args=parser.parse_args()
    if args.output is None:args.output=Path(tempfile.gettempdir())/('orca-component-world-'+uuid.uuid4().hex)
    assert not args.output.exists();args.output.mkdir(parents=True)
    print('Evidence: '+str(args.output.resolve()),flush=True)
    dll_directory=os.add_dll_directory(str(args.library.resolve().parent)) if os.name=='nt' else None
    lib=C.CDLL(str(args.library.resolve()));cb_type=C.CFUNCTYPE(None,C.c_char_p,C.c_void_p)
    lib.orca_mobile_slicing_create.argtypes=[C.c_char_p];lib.orca_mobile_slicing_create.restype=C.c_void_p
    lib.orca_mobile_slicing_execute.argtypes=[C.c_void_p,C.c_char_p,cb_type,C.c_void_p,C.POINTER(C.c_void_p)];lib.orca_mobile_slicing_execute.restype=C.c_int
    lib.orca_mobile_slicing_free_string.argtypes=[C.c_void_p];lib.orca_mobile_slicing_destroy.argtypes=[C.c_void_p]
    handle=lib.orca_mobile_slicing_create(str(args.output.resolve()).encode());assert handle
    rows=[]
    def save():
        (args.output/'results.json').write_text(json.dumps({'library':str(args.library.resolve()),'librarySha256':sha(args.library),'runs':rows,'scope':'actual host public C ABI; no independent QA/Android claim'},indent=2),encoding='utf-8')
    def execute(name,path,operation='import'):
        before=sha(path);request={'schemaVersion':2,'operation':operation,'inputPath':str(path.resolve()),'projectId':name,'projectRevision':1,'outputDir':str((args.output/name).resolve()),'quality':'standard','maxThreads':1,'config':{'before_layer_change_gcode':'G92 E0','filament_density':[1.24],'filament_diameter':[1.75]}}
        events=[]
        @cb_type
        def callback(raw,user):events.append(json.loads(raw))
        out=C.c_void_p();status=lib.orca_mobile_slicing_execute(handle,json.dumps(request).encode(),callback,None,C.byref(out))
        try:result=json.loads(C.string_at(out))
        finally:lib.orca_mobile_slicing_free_string(out)
        row={'name':name,'operation':operation,'request':request,'status':status,'result':result,'events':events,'sourceSha256':before,'sourcePreserved':sha(path)==before};rows.append(row);save()
        assert row['sourcePreserved'];return row
    try:
        cases=[]
        for name,expected in ORIGINALS.items():
            path=args.fixtures/name;assert sha(path)==expected;cases.append((name[:-4],path))
        transforms={
            'identity':IDENTITY,'translation':[1,0,0,0,1,0,0,0,1,30,0,0],
            'mirror':[-1,0,0,0,1,0,0,0,1,70,0,0],
            'rotation':[0,1,0,-1,0,0,0,0,1,90,0,0],
            'scale':[1.5,0,0,0,1.5,0,0,0,1.5,30,0,0],
            'nonuniform':[1.5,0,0,0,.75,0,0,0,2,30,30,0],
            'shear':[1,.25,0,.5,1,0,0,0,1,30,0,0]}
        for label,t in transforms.items():
            for first in (False,True):
                name=label+('-first' if first else '-shared');path=args.output/(name+'.3mf')
                make_source(path,[t,IDENTITY] if first else [IDENTITY,t]);cases.append((name,path))
        for label,outer,t in [('nested-noncommuting',[0,1,0,-1,0,0,0,0,1,120,0,0],transforms['nonuniform']),('nested-mirror',[-1,0,0,0,1,0,0,0,1,120,0,0],transforms['rotation'])]:
            path=args.output/(label+'.3mf');make_source(path,[IDENTITY,t],outer,nested=True);cases.append((label,path))
        for label,kw in [('flat-mirror',{'flat':True}),('zero-center',{'zero_center':True})]:
            path=args.output/(label+'.3mf');make_source(path,[IDENTITY,transforms['mirror']],**kw);cases.append((label,path))
        failures=[]
        for name,path in cases:
            row=execute(name,path);expected_v,expected_f=expected_scene(path)
            if row['status']!=0 or not row['result'].get('ok'):
                row['geometryPass']=False;failures.append(name);save();continue
            scene_path=next(args.output.joinpath(name).rglob('*.scene'));actual=scene(scene_path)
            vertex_ok=len(actual['vertices'])==len(expected_v) and all(abs(a-b)<=1e-5 for v,w in zip(actual['vertices'],expected_v) for a,b in zip(v,w))
            face_ok=actual['faces']==expected_f
            material_ok=actual['materials']==[0]*len(expected_f)
            row.update({'expectedVertices':expected_v,'expectedFaces':expected_f,'actualScene':actual,'vertexPass':vertex_ok,'windingPass':face_ok,'materialPass':material_ok,'geometryPass':vertex_ok and face_ok and material_ok});save()
            if not row['geometryPass']:failures.append(name)
            print(json.dumps({'case':name,'vertices':vertex_ok,'winding':face_ok,'materials':material_ok}),flush=True)
        if args.public_operations:
            for original in ORIGINALS:
                for op in ('recommend','validate','slice'):
                    row=execute(original[:-4]+'-'+op,args.fixtures/original,op)
                    ok=row['status']==0 and row['result'].get('ok')
                    if op=='recommend' and ok:
                        plans=row['result']['result']['smartSliceAnalysis']['plans'];ok=len(plans)==3 and all(not p['patch']['enable_support'] for p in plans)
                    row['operationPass']=ok;save()
                    if not ok:failures.append(row['name'])
        print(json.dumps({'cases':len(cases),'failed':failures}),flush=True)
        assert not failures,failures
    finally:lib.orca_mobile_slicing_destroy(handle)

if __name__=='__main__':main()

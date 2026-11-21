"""Deterministic exact 2,000,000 face closed torus, UV seam duplicates exact position bits."""
from pathlib import Path
import array,math,json,struct,zlib,hashlib,time
ROOT=None
def png(rgb):
 def chunk(name,data):return struct.pack('>I',len(data))+name+data+struct.pack('>I',zlib.crc32(name+data)&0xffffffff)
 return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0'+bytes(rgb)))+chunk(b'IEND',b'')
def generate(textured):
 n=1000;stride=n+1 if textured else n;positions=array.array('f');normals=array.array('f');uv=array.array('f');indices=array.array('I')
 trig=[(math.cos(2*math.pi*i/n),math.sin(2*math.pi*i/n))for i in range(n)]
 for i in range(stride):
  cu,su=trig[i%n]
  for j in range(stride):
   cv,sv=trig[j%n];positions.extend(((2+.5*cv)*cu,.5*sv,(2+.5*cv)*su))
   if textured:normals.extend((cv*cu,sv,cv*su));uv.extend((i/n,j/n))
 for i in range(n):
  for j in range(n):
   a=i*stride+j;b=((i+1)%stride)*stride+j;c=i*stride+(j+1)%stride;d=((i+1)%stride)*stride+(j+1)%stride
   indices.extend((a,c,b,b,c,d))
 binary=bytearray();views=[];accessors=[]
 def accessor(data,kind,count,component,bounds=False):
  while len(binary)%4:binary.append(0)
  view=len(views);views.append({'buffer':0,'byteOffset':len(binary),'byteLength':len(data)*data.itemsize});binary.extend(data.tobytes());a={'bufferView':view,'componentType':component,'count':count,'type':kind}
  if bounds:a.update(min=[min(data[k::3])for k in range(3)],max=[max(data[k::3])for k in range(3)])
  accessors.append(a);return len(accessors)-1
 vertex_count=len(positions)//3;attrs={'POSITION':accessor(positions,'VEC3',vertex_count,5126,True)}
 if textured:attrs['NORMAL']=accessor(normals,'VEC3',vertex_count,5126);attrs['TEXCOORD_0']=accessor(uv,'VEC2',vertex_count,5126)
 primitive={'attributes':attrs,'indices':accessor(indices,'SCALAR',len(indices),5125),'mode':4}
 doc={'asset':{'version':'2.0'},'buffers':[{'byteLength':0}],'bufferViews':views,'accessors':accessors,'meshes':[{'primitives':[primitive]}],'nodes':[{'mesh':0}],'scenes':[{'nodes':[0]}],'scene':0}
 imagehashes=[]
 if textured:
  images=[]
  for rgb in [(220,80,30),(255,110,160),(128,128,255)]:
   data=png(rgb)
   while len(binary)%4:binary.append(0)
   view=len(views);views.append({'buffer':0,'byteOffset':len(binary),'byteLength':len(data)});binary.extend(data);images.append({'bufferView':view,'mimeType':'image/png'});imagehashes.append(hashlib.sha256(data).hexdigest())
  primitive['material']=0;doc.update(images=images,textures=[{'source':i,'sampler':0}for i in range(3)],samplers=[{'wrapS':10497,'wrapT':33071}],materials=[{'pbrMetallicRoughness':{'baseColorTexture':{'index':0},'metallicRoughnessTexture':{'index':1},'metallicFactor':.7,'roughnessFactor':.4},'normalTexture':{'index':2,'scale':.8},'alphaMode':'BLEND','doubleSided':True}])
 doc['buffers'][0]['byteLength']=len(binary)
 while len(binary)%4:binary.append(0)
 js=json.dumps(doc,separators=(',',':')).encode();js+=b' '*(-len(js)%4);target=ROOT/('torus-2m-pbr-normal-seam.glb'if textured else'torus-2m-untextured.glb')
 with target.open('xb') as f:f.write(struct.pack('<IIIII',0x46546c67,2,28+len(js)+len(binary),len(js),0x4e4f534a));f.write(js);f.write(struct.pack('<II',len(binary),0x004e4942));f.write(binary)
 return {'path':str(target),'bytes':target.stat().st_size,'sha256':hashlib.file_digest(target.open('rb'),'sha256').hexdigest(),'vertices':vertex_count,'faces':len(indices)//3,'images':imagehashes,'analyticClosedManifold':True,'grid':[n,n],'seamPositionCoordinatesModuloGrid':True,'authoredNormals':textured}
if __name__=='__main__':
 import argparse
 parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();ROOT=args.output.resolve()
 ROOT.mkdir(parents=True,exist_ok=True);result=[generate(False),generate(True)];(ROOT/'fixtures.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))

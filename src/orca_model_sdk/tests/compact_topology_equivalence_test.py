from pathlib import Path
import subprocess,json,hashlib
import argparse,os
parser=argparse.ArgumentParser();parser.add_argument('--workspace',type=Path,required=True);parser.add_argument('--evidence',type=Path,required=True);args=parser.parse_args()
r=Path(__file__).resolve().parents[3];e=args.evidence.resolve();e.mkdir(parents=True,exist_ok=True);out=args.workspace.resolve();out.mkdir()
s=(r/'src/orca_model_sdk/glb_safety.cpp').read_text();start=s.index('std::array<size_t,5> compact_topology_counts(');end=s.index('\nTopology topology(',start);function=s[start:end]
pre=r"""#include <array>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <random>
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <cstdint>
using V=std::array<double,3>;using T=std::array<uint32_t,3>;
struct Context { bool canceled=false; };
void check_cancel(Context& c){if(c.canceled)throw std::runtime_error("CANCELED");}
"""
post=r"""
std::array<size_t,5> reference(const std::vector<V>& v,const std::vector<T>& f) {
 std::set<T> unique;std::map<std::pair<uint32_t,uint32_t>,std::array<size_t,2>> edges;size_t deg=0,dup=0,boundary=0,nonmanifold=0,inconsistent=0;
 for(auto face:f){auto a=v.at(face[0]),b=v.at(face[1]),d=v.at(face[2]);V u{b[0]-a[0],b[1]-a[1],b[2]-a[2]},w{d[0]-a[0],d[1]-a[1],d[2]-a[2]};if(V{u[1]*w[2]-u[2]*w[1],u[2]*w[0]-u[0]*w[2],u[0]*w[1]-u[1]*w[0]}==V{})++deg;auto key=face;std::sort(key.begin(),key.end());if(!unique.insert(key).second)++dup;for(size_t k=0;k<3;++k){auto x=face[k],y=face[(k+1)%3];++edges[{std::min(x,y),std::max(x,y)}][x<y?0:1];}}
 for(auto entry:edges){auto use=entry.second;if(use[0]+use[1]==1)++boundary;if(use[0]+use[1]>2)++nonmanifold;if(use[0]+use[1]==2&&(use[0]==2||use[1]==2))++inconsistent;}
 return {deg,dup,boundary,nonmanifold,inconsistent};
}
int main(){Context c;std::mt19937 rng(20261119);std::vector<V> raw{{-0.,0.,0.},{0.,0.,0.},{1,0,0},{0,1,0},{0,0,1},{1,0,0},{2,2,2}};
 for(size_t round=0;round<512;++round){std::vector<T> faces;for(size_t i=0;i<round%41;++i)faces.push_back({uint32_t(rng()%raw.size()),uint32_t(rng()%raw.size()),uint32_t(rng()%raw.size())});
  // Old dense first-occurrence canonical IDs and new first-index IDs must both
  // preserve all geometric/edge facts, including signed zero and duplicates.
  std::map<V,uint32_t> ids,first;std::vector<V> dense;std::vector<uint32_t> remap,firstmap;
  for(size_t i=0;i<raw.size();++i){auto a=ids.emplace(raw[i],uint32_t(dense.size()));if(a.second)dense.push_back(raw[i]);remap.push_back(a.first->second);firstmap.push_back(first.emplace(raw[i],uint32_t(i)).first->second);}
  std::vector<T> df,ff;for(auto f:faces){df.push_back({remap[f[0]],remap[f[1]],remap[f[2]]});ff.push_back({firstmap[f[0]],firstmap[f[1]],firstmap[f[2]]});}
  auto before=df;auto raw_before=raw;auto wanted=reference(dense,df);if(compact_topology_counts(c,dense,df)!=wanted || compact_topology_counts(c,raw,ff)!=wanted || df!=before || std::memcmp(raw.data(),raw_before.data(),raw.size()*sizeof(V)))return 1;
 }
 c.canceled=true;bool rejected=false;try{compact_topology_counts(c,raw,{{0,2,3}});}catch(const std::runtime_error& x){rejected=std::string(x.what())=="CANCELED";}if(!rejected)return 2;
 std::cout<<"PASS 512 exact production-helper versus old tree reference + cancellation; source vertices/face order unchanged"<<std::endl;
}
"""
(out/'test.cpp').write_text(pre+function+post);(out/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.22)\nproject(counts LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 17)\nadd_executable(counts test.cpp)\n')
configure=['cmake','-S',str(out),'-B',str(out/'build'),'-DCMAKE_BUILD_TYPE=Release']
if os.name=='nt':configure+=['-G','Visual Studio 17 2022','-A','x64']
executable=out/('build/Release/counts.exe' if os.name=='nt' else 'build/counts')
commands=[configure,['cmake','--build',str(out/'build'),'--config','Release'],[str(executable)]]
with (e/'compact-counts-equivalence.log').open('wb') as log:
 for command in commands:
  rc=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT).returncode
  if rc:raise SystemExit(rc)
(e/'compact-counts-equivalence.json').write_text(json.dumps({'productionFunctionSha256':hashlib.sha256(function.encode()).hexdigest(),'compileExactFunctionFrom':str(r/'src/orca_model_sdk/glb_safety.cpp'),'cases':512,'cancellationCase':1,'reference':'old set/map counter and dense first-occurrence vertex identity','isNativeMathOnly':True,'notPublicCABIOrAuthGuardMock':True,'commands':commands},indent=2))
print('PASS 512 count equivalence plus cancel')

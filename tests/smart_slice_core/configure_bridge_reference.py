"""Generate a read-only native dependency config from a source-matched MSVC build.
This is a development regression harness, never a qualified SDK producer.
Usage: python configure_bridge_reference.py --reference-build <build> --output <cmake>
"""
import argparse, pathlib, subprocess, re, json, hashlib, xml.etree.ElementTree as ET
p=argparse.ArgumentParser();p.add_argument('--reference-build',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[2];build=a.reference_build.resolve()
cache=(build/'CMakeCache.txt').read_text()
home=next(line.split('=',1)[1] for line in cache.splitlines() if line.startswith('CMAKE_HOME_DIRECTORY:'))
reference=pathlib.Path(home).parent.resolve()
# Every reused Core implementation and public header must match this checkout.
changes=subprocess.check_output(['git','diff','--name-only','273fbf5fe948f51ddffe9d1f85dcc540e7b79bab','--','src/libslic3r','deps_src'],cwd=root,text=True)
assert not changes, changes
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=reference,text=True).strip()=='273fbf5fe948f51ddffe9d1f85dcc540e7b79bab'
assert not subprocess.check_output(['git','status','--porcelain','--','src/libslic3r','deps_src'],cwd=reference,text=True).strip()
ns={'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
x=ET.parse(build/'orca_mobile_slicing_sdk.vcxproj').getroot()
g=next(g for g in x.findall('m:ItemDefinitionGroup',ns) if 'Release' in g.get('Condition',''))
def values(tag):return [v for v in g.find('.//m:'+tag,ns).text.split(';') if v and not v.startswith('%(')]
def newpath(s):
 s=s.replace('\\','/')
 # Submodule source dependencies remain pinned and read-only in the reference.
 if '/deps_src/' in s:return s
 return s.replace(str(reference).replace('\\','/'),str(root).replace('\\','/'))
includes=[newpath(v) for v in values('AdditionalIncludeDirectories')]
includes += [newpath(v) for v in re.findall(r'/external:I "([^"]+)"',g.find('.//m:AdditionalOptions',ns).text)]
defs=[v for v in values('PreprocessorDefinitions') if 'SOURCE_COMMIT' not in v and 'EXPORTS' not in v and 'CMAKE_INTDIR' not in v and v!='ORCA_SLICING_BUILD']
libs=[]
for v in values('AdditionalDependencies'):
 if 'orcaslicer_ai_smart_slicing.lib' in v:continue
 v=v.replace('\\','/')
 libs.append(str((build/v).resolve()).replace('\\','/') if '/' in v else v)
def emit(name,items):return 'set('+name+'\n'+''.join('  [=['+v+']=]\n' for v in items)+')\n'
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(emit('ORCA_BRIDGE_INCLUDES',includes)+emit('ORCA_BRIDGE_DEFINITIONS',defs)+emit('ORCA_BRIDGE_LIBRARIES',libs))
print('Generated development-only reference config:',a.output)

# Record the precise native libraries reused by the development harness.
manifest={'referenceCommit':'273fbf5fe948f51ddffe9d1f85dcc540e7b79bab','referenceBuild':str(build),'kind':'development bridge regression only','libraries':[]}
for lib in libs:
 q=pathlib.Path(lib)
 if q.is_absolute():
  assert q.is_file(), q
  manifest['libraries'].append({'path':str(q),'sha256':hashlib.sha256(q.read_bytes()).hexdigest()})
a.output.with_suffix('.json').write_text(json.dumps(manifest,indent=2))

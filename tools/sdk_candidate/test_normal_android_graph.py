"""Tiny graph-entry coverage regressions; synthetic profiles, no native tools."""
import contextlib,copy,hashlib,json,tempfile,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import normal_android_producer as n
import adapter as a
import package_android as p

class GraphDependencyCoverage(unittest.TestCase):
    @contextlib.contextmanager
    def fixture(self,mode='valid',changed=False,rebuild=False,new_header=False):
        with tempfile.TemporaryDirectory() as temp,contextlib.ExitStack() as stack:
            base=Path(temp);cache=base/'cache';cache.mkdir();source_root=base/'source';source_root.mkdir()
            stage='model-android';root=base/'evidence'/stage;root.mkdir(parents=True)
            oldroot=base/'old'/'prior';oldroot.mkdir(parents=True)
            def write(path,value):
                path.parent.mkdir(parents=True,exist_ok=True)
                if isinstance(value,dict) or isinstance(value,list):path.write_text(json.dumps(value),encoding='utf-8')
                else:path.write_text(value,encoding='utf-8')
                return a.identity(path)
            stack.enter_context(patch.dict(n.CACHES,{stage:str(cache),'slicing-android':str(base/'other')},clear=True))
            stack.enter_context(patch.object(n,'BASE',base/'old'))
            header=source_root/'source.h';write(header,'new' if changed else 'old')
            obj=cache/'a.o';objref=write(obj,'object');objrow=dict(objref)
            old_header=dict(a.identity(header));old_header['sha256']=hashlib.sha256(b'old').hexdigest()
            old_header['bytes']=3
            old_source=write(oldroot/'actual-compiler-inputs.json',dict(readInputs=[old_header,objrow]))
            old_objects=write(oldroot/'new-object-inventory.json',[objrow])
            old_seal=write(oldroot/'key-seal.json',dict(refs=[old_source,old_objects]))
            stack.enter_context(patch.dict(n.BASELINES,{stage:('prior','key-seal.json',old_seal['sha256'],old_objects['sha256'],1)}))
            frozen=dict(stage=stage,sourceCommit=n.SOURCE,newBuildRoot=str(cache),
                configureArgv=['cmake','-DCMAKE_MAKE_PROGRAM:FILEPATH=ninja',
                  '-DORCA_MODEL_ANDROID_TBB_RUNTIME:FILEPATH='+str(base/'vendor/libtbb.so')],
                buildArgv=['cmake','--build',str(cache),'--target','orca_model_sdk','--parallel','1'])
            normal=write(base/'normal-plan.json',dict(codeRef=n.SOURCE,stages={stage:frozen,'slicing-android':{}}))
            go=write(base/'go.json',dict(source=n.SOURCE,sourceTree=n.TREE,authorized=True,plan=normal))
            stack.enter_context(patch.object(n,'GO_SHA',go['sha256']))
            raw_file=write(root/'raw/liborca_model_sdk_c.so','synthetic bytes, never loaded')
            build_file=write(cache/'liborca_model_sdk_c.so','synthetic bytes, never loaded')
            raw=dict(schema=n.SCHEMA,stage=stage,source=n.SOURCE,sourceTree=n.TREE,sourceRoot=str(source_root),
                ownedCache=str(cache),raw=raw_file,buildRaw=build_file,buildPath=build_file['path'],cleanBuild=False,
                copiedPredecessorObjects=0,priorObjectsReused=True,go=go,normalPlan=normal)
            rows=[dict(name=stage+'-'+kind,argv=frozen[kind+'Argv'],cwd=str(source_root),rootPid=123,
                exitCode=0,started='2026-11-27T00:00:00Z',ended='2026-11-27T00:00:01Z') for kind in ['configure','build']]
            exe=write(root.parent/'execution.json',dict(source=n.SOURCE,sourceTree=n.TREE,copiedObjects=0,
                memoryViolations=[],commands=rows))
            term=dict(schema=n.TERMINAL_SCHEMA,source=n.SOURCE,sourceClean=True,sdkExecuted=False,naturalReturn=True,
                cancelled=False,memoryViolation=[],minimumFreeBytes=10000000000,commands=rows,executionRef=exe)
            rawref=write(root/'raw-source.json',raw);termref=write(root/'actual-terminal.json',term)
            headrow=a.identity(header);headrow['sourceBinding']=dict(sourceCommit=n.SOURCE,rawBytes=3,rawSha256=headrow['sha256'])
            sourcerows=[headrow,objrow]
            if new_header:sourcerows.append(write(source_root/'added.h','added'))
            srcref=write(root/'actual-compiler-inputs.json',dict(source=n.SOURCE,readInputs=sourcerows))
            objref=write(root/'new-object-inventory.json',[objrow]);flagref=write(root/'actual-source-command-flags.json',{})
            valid='a.o: #deps 1, deps mtime 1 (VALID)\n    '+str(header)+'\n'
            before=write(root/'before/ninja-deps.txt',valid)
            current=valid
            if new_header:current=valid.replace('#deps 1','#deps 2')+'    '+str(source_root/'added.h')+'\n'
            if mode=='missing':current=''
            if mode=='stale':current=valid.replace('(VALID)','(STALE)')
            if mode=='partial':current=valid.replace('#deps 1','#deps 2')
            if mode=='empty':current='a.o: #deps 0, deps mtime 1 (VALID)\n'
            after=write(root/'after/ninja-deps.txt',current+
                'liborca_model_sdk_c.so: #deps 1, deps mtime 1 (VALID)\n    a.o\n')
            log=write(root.parent/(stage+'-build.log'),('[1/2] Building CXX object a.o\n'
                '[2/2] Linking CXX shared library liborca_model_sdk_c.so\n') if rebuild else '')
            cmds=write(root/'approved-target-commands.txt','clang -o a.o -c source.cpp\nclang -o liborca_model_sdk_c.so a.o\n')
            dot=write(root/'approved-target-graph.txt','digraph ninja {\n"o" [label="a.o"]\n"s" [label="liborca_model_sdk_c.so"]\n}\n')
            ops=[dict(argv=['ninja','-C',str(cache),'-t',kind,'orca_model_sdk'],cwd=str(cache),exitCode=0,output=ref)
                 for kind,ref in [('commands',cmds),('graph',dot)]]
            domain=write(root/'approved-target-object-domain.json',dict(scope='synthetic',targets=['orca_model_sdk'],operations=ops,objects=[]))
            historical=[old_seal,old_source,old_objects]
            origin=write(root/'normal-incremental-origin.json',dict(schema='orca-android-normal-incremental-origin-v1',
                source=n.SOURCE,sourceTree=n.TREE,stage=stage,rawSourceRef=rawref,terminalRef=termref,
                currentSourceInputsRef=srcref,physicalPostInventoryRef=objref,currentNinjaTargetDomainRef=domain,
                qualifiedBaselineRefs=historical))
            refs=[rawref,termref,srcref,objref,flagref,before,after,log,cmds,dot,domain,origin,exe]
            sealed=dict(refs=refs);seal=write(root.parent/'key-seal.json',sealed)
            item=dict(stage=stage,rawSource=raw,terminal=term,seal=seal,fullIndex=sealed,normalProvenance=dict(
                origin=origin,historicalMetadata=historical,targetDomain=domain,beforeDeps=before,afterDeps=after,buildLog=log))
            plan=dict(producers={stage:item},resourceSummaries={stage:termref})
            api=SimpleNamespace(small_json=a.small_json,verify=a.verify,canonical=p.typed_json,
                exact_refs=p.exact_refs,pins=p.pins,original_refs=p.original_refs)
            expected=dict(seal=seal['sha256'],originSha=origin['sha256'],rawSha=raw_file['sha256'],rawBytes=raw_file['bytes'])
            yield lambda:n.graph(plan,stage,api,expected)

    def test_retained_object_graph_positive(self):
        with self.fixture() as run:
            item,source,refs=run();self.assertTrue(refs);self.assertEqual(len(source['readInputs']),2)

    def test_current_retained_missing_stale_empty_partial_graph_rejected(self):
        for mode in ['missing','stale','empty','partial']:
            with self.subTest(mode=mode),self.fixture(mode=mode) as run,self.assertRaises(ValueError):run()

    def test_known_changed_dependency_graph_requires_actual_rebuild(self):
        with self.fixture(changed=True) as run,self.assertRaisesRegex(ValueError,'actual rebuild'):run()
        with self.fixture(changed=True,rebuild=True) as run:self.assertTrue(run()[2])

    def test_new_transitive_header_graph_requires_rebuild(self):
        with self.fixture(new_header=True) as run,self.assertRaisesRegex(ValueError,'actual rebuild'):run()
        with self.fixture(new_header=True,rebuild=True) as run:self.assertTrue(run()[2])

if __name__=='__main__':unittest.main()

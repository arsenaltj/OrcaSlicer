"""Use root-sealed real metadata only; never open any producer binary."""
from pathlib import Path
import copy
import io
import json
import tempfile
import unittest
from unittest.mock import patch
import adapter as a
import companion as c

E=Path('D:/Codex/artifacts/task-20261125-b5f-sdk-package-tooling/evidence')


class RealMetadataFix3(unittest.TestCase):
    def metadata(self):
        seal=a.small_json(E/'se-real-metadata-reachability-v1/seal.json')
        refs=seal['files']
        lookup={str(Path(r['path']).resolve()):r for r in refs}
        field=a.small_json(E/'se-real-binding-fieldmap.json')
        plan=next(r for r in refs if Path(r['path']).name=='clean-producer-plan.json')
        return refs,lookup,field,plan

    def test_three_actual_cache_basis_and_strict_types(self):
        refs,lookup,field,plan=self.metadata()
        for stage in ('model-windows','model-android','slicing-android'):
            with self.subTest(stage=stage):
                producer=dict(rawSource=field['stages'][stage]['rawSource'],fullIndex={'files':[]},originalRecords={})
                cache=Path(field['stages'][stage]['cacheCandidatePath'])
                entry=dict(role='cache',stage=stage,producerBuildRoot=str(cache.parent),
                           current=a.identity(cache),basis=dict(normalPlan=plan))
                c.basis(entry,producer,stage,[],[plan])
        self.assertTrue(c.frozen_value('D:/fixed/path','D:\\fixed\\path','PATH'))
        for kind in ('STRING','BOOL','UNINITIALIZED',''):
            self.assertFalse(c.frozen_value('D:/fixed/path','D:\\fixed\\path',kind))
        for actual in ('E:/fixed/path','D:/different/path',None,''):
            self.assertFalse(c.frozen_value(actual,'D:\\fixed\\path','FILEPATH'))

    def records(self):
        refs,lookup,field,plan=self.metadata()
        producer=dict(rawSource=field['stages']['model-windows']['rawSource'],fullIndex={'files':[]},originalRecords={})
        build=c.build_root(producer,'model-windows')
        sdk=build/'orca_model_sdk.vcxproj';dep=build/'repair-deps/tbb/src/tbb/tbb.vcxproj'
        runtime=build/'msvc_19.44_cxx17_64_md_release/tbb12.dll';lib=runtime.with_suffix('.lib')
        ref=lambda p:lookup[str(Path(p).resolve())]
        rows=[dict(role=role,stage='model-windows',target='orca_model_sdk',configuration='Release|x64',
                   pin=ref(build/('orca_model_sdk.dir/Release/orca_model_sdk.tlog/link.'+role+'.1.tlog')))
              for role in ('command','read','write')]
        return refs,producer,plan,build,sdk,dep,runtime,lib,rows,ref

    def test_actual_three_record_basis_metadata_reaches_binary_boundary(self):
        refs,producer,plan,build,sdk,dep,runtime,lib,rows,ref=self.records()
        entry=dict(role='runtime',stage='model-windows',producerBuildRoot=str(build),
                   current=dict(path=str(runtime),bytes=0,sha256='0'*64),basis=dict(normalPlan=plan,
                    dependencyProject=ref(dep),dependencyTarget='tbb',dependencyImportLib=str(lib),project=ref(sdk),
                    linkTlog=ref(dep.parent/'tbb.dir/Release/tbb.tlog/link.command.1.tlog'),sdkLinkRecords=rows))
        original=Path.open;guard=[]
        def opened(path,*args,**kwargs):
            if path.resolve()==runtime.resolve():
                guard.append(str(path));return io.BytesIO(b'MZ_TEST_ONLY_NO_REAL_BINARY')
            return original(path,*args,**kwargs)
        with patch.object(Path,'open',opened):c.basis(entry,producer,'model-windows',[],refs)
        self.assertEqual(guard,[str(runtime)])
        tokens=c.sdk_dependency(sdk,dep,str(lib))
        proof=c.linked_records(rows,lambda r:a.verify(r),sdk,'model-windows',str(lib),producer['rawSource']['buildPath'],tokens)
        self.assertEqual(proof['recordKeyBytes'],1978)
        self.assertEqual(proof['recordKeySha256'],'caba28d96c65ca26b46372ad2464d67f887010fe865d936419544e34d9b04f5b')
        self.assertNotIn('sdkLinkTlog',entry['basis'])
        bad=copy.deepcopy(entry);bad['basis']['sdkLinkTlog']=rows[0]['pin']
        with self.assertRaisesRegex(ValueError,'conflicting/missing'):c.basis(bad,producer,'model-windows',[],refs)

    def test_linked_records_wrong_key_stage_config_missing_ambiguity_and_identity(self):
        refs,producer,plan,build,sdk,dep,runtime,lib,rows,ref=self.records()
        tokens=c.sdk_dependency(sdk,dep,str(lib))
        for field,value in (('stage','slicing-windows'),('target','other'),('configuration','Debug|x64')):
            bad=copy.deepcopy(rows);bad[0][field]=value
            with self.assertRaises(ValueError):c.linked_records(bad,a.verify,sdk,'model-windows',str(lib),producer['rawSource']['buildPath'],tokens)
        with self.assertRaises(ValueError):c.linked_records(rows[:2],a.verify,sdk,'model-windows',str(lib),producer['rawSource']['buildPath'],tokens)
        for wronglib,wrongout in ((str(lib)+'.wrong',producer['rawSource']['buildPath']), (str(lib),producer['rawSource']['buildPath']+'.wrong')):
            with self.assertRaises(ValueError):c.linked_records(rows,a.verify,sdk,'model-windows',wronglib,wrongout,tokens)
        with tempfile.TemporaryDirectory() as d:
            temp=Path(d);fake_sdk=temp/'orca_model_sdk.vcxproj';fake_sdk.write_text('<Project/>')
            folder=temp/'orca_model_sdk.dir/Release/orca_model_sdk.tlog';folder.mkdir(parents=True)
            for variant in ('wrongkey','multiple','wronglib','wrongoutput'):
                copies=[]
                for row in rows:
                    role=row['role'];text=Path(row['pin']['path']).read_bytes().decode('utf-16')
                    if role=='read' and variant=='wrongkey':text=text.replace('^','^OTHER',1)
                    if role=='read' and variant=='multiple':text+='\n^EXTRA_RECORD\nfile\n'
                    if role=='read' and variant=='wronglib':text=text.replace(str(lib).upper(),str(lib).upper()+'.WRONG')
                    if role=='write' and variant=='wrongoutput':text=text.replace('ORCA_MODEL_SDK_C.DLL','OTHER.DLL')
                    path=folder/('link.'+role+'.1.tlog');path.write_text(text,encoding='utf8')
                    copies.append(dict(row,pin=a.identity(path)))
                with self.subTest(variant=variant),self.assertRaises(ValueError):
                    c.linked_records(copies,a.verify,fake_sdk,'model-windows',str(lib),producer['rawSource']['buildPath'],tokens)


if __name__=='__main__':unittest.main()

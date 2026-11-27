import copy
import unittest
from pathlib import Path
import normal_android_producer as n

class NormalProducerTests(unittest.TestCase):
    def fixture(self,stage='model-android'):
        raw=dict(schema=n.SCHEMA,source=n.SOURCE,sourceTree=n.TREE,
            cleanBuild=False,copiedPredecessorObjects=0,priorObjectsReused=True,
            buildPath=str(Path(n.CACHES[stage])/'sdk.so'),ownedCache=n.CACHES[stage],go=dict(sha256=n.GO_SHA))
        terminal=dict(schema=n.TERMINAL_SCHEMA,source=n.SOURCE,sourceClean=True,sdkExecuted=False,
            naturalReturn=True,cancelled=False,memoryViolation=[],minimumFreeBytes=10000000000,
            commands=[dict(name=stage+'-'+name,rootPid=123,exitCode=0,started='2026-11-27T00:00:00Z',
                ended='2026-11-27T00:00:01Z') for name in ['configure','build']])
        return terminal,raw

    def test_exact_normal_both_android_stages(self):
        for stage in n.CACHES:
            terminal,raw=self.fixture(stage)
            self.assertTrue(n.selected(terminal));self.assertTrue(n.validate_terminal(stage,terminal,raw))

    def test_wrong_stage_schema_source_tree_go_cache_and_copy(self):
        for field,value in [('schema','other'),('source','0'*40),('sourceTree','0'*40),
            ('cleanBuild',True),('cleanBuild',0),('copiedPredecessorObjects',False),
            ('copiedPredecessorObjects',1),('priorObjectsReused',1),('ownedCache','D:/foreign')]:
            terminal,raw=self.fixture();raw[field]=value
            with self.subTest(field=field,value=value),self.assertRaises(ValueError):n.validate_terminal('model-android',terminal,raw)
        terminal,raw=self.fixture();raw['go']['sha256']='0'*64
        with self.assertRaises(ValueError):n.validate_terminal('model-android',terminal,raw)
        with self.assertRaises(ValueError):n.validate_terminal('model-windows',terminal,raw)

    def test_unknown_schema_never_falls_back_to_old_recovery(self):
        self.assertFalse(n.selected({}))
        for value in [n.SCHEMA,'unknown',False,0]:
            with self.subTest(value=value),self.assertRaises(ValueError):n.selected(dict(schema=value))

    def test_resource_return_and_typed_negative_cases(self):
        for field,value in [('sourceClean',False),('sdkExecuted',True),('naturalReturn',False),
            ('cancelled',True),('memoryViolation',[{}]),('memoryViolation',False),
            ('minimumFreeBytes',3221225472),('minimumFreeBytes',True),('commands',[])]:
            terminal,raw=self.fixture();terminal[field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):n.validate_terminal('model-android',terminal,raw)
        for field,value in [('exitCode',False),('exitCode',1),('rootPid',False),('rootPid',0),
            ('started',''),('ended',None),('name','build')]:
            terminal,raw=self.fixture();terminal['commands'][0][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):n.validate_terminal('model-android',terminal,raw)

    def row(self,name,digest='1'*64,size=1):
        return dict(path=str(Path(n.CACHES['model-android'])/name),bytes=size,sha256=digest)

    def test_retained_rebuilt_unchanged_changed_and_new(self):
        root=n.CACHES['model-android'];rows=[self.row('a.o'),self.row('b.o'),self.row('c.o')]
        old=n.inventory(rows,root)
        new=n.inventory([rows[0],rows[1],self.row('c.o','2'*64),self.row('d.o')],root)
        rebuilt={n.key(r['path']) for r in [rows[1],self.row('c.o'),self.row('d.o')]}
        result=n.classify(old,new,rebuilt)
        self.assertEqual({k:len(v) for k,v in result.items()},dict(retained=1,rebuiltUnchanged=1,changed=1,new=1))

    def test_changed_new_missing_and_unpublished_rebuild_reject(self):
        root=n.CACHES['model-android'];old=n.inventory([self.row('a.o')],root)
        for rows,actions in [([self.row('a.o','2'*64)],set()),
            ([self.row('a.o'),self.row('new.o')],set()),([self.row('new.o')],{n.key(self.row('new.o')['path'])}),
            ([self.row('a.o')],{n.key(self.row('absent.o')['path'])})]:
            with self.subTest(rows=rows),self.assertRaises(ValueError):n.classify(old,n.inventory(rows,root),actions)

    def test_object_shape_duplicates_and_foreign_reject(self):
        root=n.CACHES['model-android'];row=self.row('a.o')
        for rows in [[],[row,row],[dict(row,bytes=True)],[dict(row,sha256='x')],
            [dict(row,path='D:/foreign/a.o')],[dict(row,path=str(Path(root)/'a.cpp'))]]:
            with self.subTest(rows=rows),self.assertRaises(ValueError):n.inventory(rows,root)

    def test_actions_are_actual_build_lines_not_command_graph(self):
        root=n.CACHES['model-android']
        result=n.actions('[1/4] Building CXX object CMakeFiles/sdk.dir/a.cpp.o\n'
            '[2/4] Building C object lib/b.c.o\n[3/4] Linking CXX shared library sdk.so\n'
            'clang++ -o never.o -c source.cpp\n',root)
        self.assertEqual(result,{n.key(Path(root)/'CMakeFiles/sdk.dir/a.cpp.o'),n.key(Path(root)/'lib/b.c.o')})
        with self.assertRaises(ValueError):n.actions('[1/1] Building CXX object ../foreign.o',root)

    def test_changed_input_dependencies_require_actual_rebuild(self):
        root=n.CACHES['model-android'];obj=n.key(Path(root)/'a.o');header=n.key(Path(root)/'generated.h')
        old={'readInputs':[dict(path=header,bytes=1,sha256='1'*64)]}
        new={'readInputs':[dict(path=header,bytes=1,sha256='2'*64)]}
        before={obj:{header}};after={obj:{header}}
        with self.assertRaises(ValueError):n.input_rebuilds(old,new,before,after,set(),{obj})
        result=n.input_rebuilds(old,new,before,after,{obj},{obj})
        self.assertEqual(result['affectedObjects'],[obj])
        self.assertEqual(n.input_rebuilds(old,old,before,after,set(),{obj})['affectedObjects'],[])

    def test_ninja_dependencies_keep_only_valid_object_sections(self):
        root=n.CACHES['model-android'];text='a.o: #deps 1, deps mtime 123 (VALID)\n    input.h\n\n'
        self.assertEqual(n.dependencies(text,root),{n.key(Path(root)/'a.o'):{n.key(Path(root)/'input.h')}})
        self.assertEqual(n.dependencies(text.replace('(VALID)','(STALE)'),root),{})

if __name__=='__main__':unittest.main()

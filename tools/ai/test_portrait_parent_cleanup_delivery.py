from copy import deepcopy
import unittest

from beauty_leaf_domain import digest
from portrait_parent_cleanup import POLICY
from portrait_parent_cleanup_delivery import validate_binding


class CandidateBindings(unittest.TestCase):
    def fixture(self):
        partition = dict(source_sha256='a'*64,geometry_id='b'*64,partition_sha256='c'*64)
        ref = dict(path='surface-partitions/x.json')
        locks = dict(partition_ref=ref)
        old = dict(palette=[dict(uid='portrait-skin',rgb=[1,.9,.8])],weights={'source_error':1})
        modules = {'module.py':'d'*64}
        runtime = dict(network='offline',python='3.12')
        runtime['sha256'] = digest(runtime)
        plan = dict(schema='orca.portrait-color-plan/v1',source_sha256='a'*64,geometry_id='b'*64,
                    boundary_sha256='c'*64,partition_ref=ref,shape_lock_sha256='e'*64,
                    ownership_ref=dict(schema='orca.portrait-surface-ownership-reference/v2',
                                       path='portrait-ownership/'+'f'*64+'.json',sha256='f'*64),
                    policy=deepcopy(POLICY),modules=modules,policy_sha256=digest(dict(policy=POLICY,modules=modules)),
                    runtime=runtime,runtime_sha256=runtime['sha256'],**old)
        return plan,partition,locks,old,modules

    def check(self, plan, partition, locks, old, modules):
        return validate_binding(plan,partition,locks,old,'f'*64,'e'*64,modules)

    def test_current_bound_candidate_is_valid(self):
        self.assertTrue(self.check(*self.fixture()))

    def test_source_boundary_runtime_policy_and_module_drift_fail_closed(self):
        for field in ('source_sha256','geometry_id','boundary_sha256','shape_lock_sha256',
                      'policy_sha256','runtime_sha256'):
            plan,*rest = self.fixture()
            plan[field] = '0'*64
            with self.subTest(field=field),self.assertRaises(ValueError):
                self.check(plan,*rest)
        plan,*rest = self.fixture()
        plan['modules'] = {'module.py':'0'*64}
        with self.assertRaises(ValueError):
            self.check(plan,*rest)

    def test_ownership_reference_traversal_is_rejected(self):
        plan,*rest = self.fixture()
        plan['ownership_ref']['path'] = '../source.glb'
        with self.assertRaises(ValueError):
            self.check(plan,*rest)


if __name__=='__main__':
    unittest.main()

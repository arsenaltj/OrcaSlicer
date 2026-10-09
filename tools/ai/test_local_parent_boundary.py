import unittest

import numpy as np

from local_parent_boundary import compatible_proposals,refine


class SourceParentBoundary(unittest.TestCase):
    def test_source_refinement_completes_arm_and_keeps_cloth_hair_separate(self):
        rgb=np.full((64,64,3),180,np.uint8)
        rgb[4:60,4:20]=[35,30,32];rgb[4:60,20:40]=[238,234,240];rgb[4:60,40:60]=[200,150,130]
        foreground=np.zeros((64,64),bool);foreground[4:60,4:60]=True
        labels=np.zeros((64,64),np.uint8);quality=np.zeros((64,64),float);known=np.zeros((64,64),np.uint8)
        known[20:30,8:15]=1;known[20:30,25:35]=4;known[20:30,45:55]=3
        selected,confidence,report=refine(rgb,foreground,labels,quality,known)
        self.assertTrue(np.all(selected[6:58,6:18]==1))
        self.assertTrue(np.all(selected[6:58,22:38]==4))
        self.assertTrue(np.all(selected[6:58,42:58]==3))
        self.assertFalse(selected[~foreground].any())
        self.assertTrue(report['mask_quality_is_coverage_not_calibrated_model_confidence'])
        self.assertTrue(np.all(confidence[selected>0]==1))

    def test_no_legitimate_seed_cannot_create_region_from_color(self):
        rgb=np.full((64,64,3),[200,150,130],np.uint8)
        selected,_,_=refine(rgb,np.ones((64,64),bool),np.zeros((64,64),np.uint8),np.zeros((64,64)),np.zeros((64,64),np.uint8))
        self.assertFalse(selected.any())

    def test_confident_wrong_body_class_cannot_force_hair_into_cloth(self):
        rgb=np.full((64,64,3),180,np.uint8)
        rgb[4:60,4:32]=[35,30,32];rgb[4:60,32:60]=[238,234,240]
        foreground=np.zeros((64,64),bool);foreground[4:60,4:60]=True
        labels=np.zeros((64,64),np.uint8);labels[foreground]=4
        known=np.zeros(labels.shape,np.uint8);known[20:30,8:18]=1;known[20:30,40:50]=4
        selected,_,audit=refine(rgb,foreground,labels,np.full(labels.shape,.99),known)
        self.assertTrue(np.all(selected[6:58,6:30]==1))
        self.assertTrue(np.all(selected[6:58,34:58]==4))
        self.assertGreater(audit['model_seeds_demoted_by_verified_source'],0)

    def test_missing_competing_source_models_cannot_approve_proposals(self):
        proposal=np.array([[1]],np.uint8)
        self.assertFalse(compatible_proposals(proposal,{1:np.array([[0.]])}).any())
        scores={1:np.array([[0.]]),3:np.array([[-5.]]),4:np.array([[np.nan]])}
        self.assertFalse(compatible_proposals(proposal,scores).any())

    def test_proposals_are_soft_and_original_source_vetoes_wrong_cloth_claim(self):
        rgb=np.full((64,64,3),180,np.uint8);rgb[4:60,4:60]=[35,30,32]
        foreground=np.zeros((64,64),bool);foreground[4:60,4:60]=True
        labels=np.full((64,64),4,np.uint8);confidence=np.full((64,64),.99)
        known=np.zeros((64,64),np.uint8);proposal=np.ones((64,64),np.uint8)
        reference=np.repeat(np.array([[35,30,32],[238,234,240],[200,150,130]],np.uint8),40,axis=0)[:,None,:]
        classes=np.repeat([1,4,3],40).astype(np.uint8)[:,None]
        selected,_,report=refine(rgb,foreground,labels,confidence,known,proposal,(reference,classes))
        self.assertTrue(np.all(selected[foreground]==1))
        self.assertTrue(report['all_parent_source_models_available'])
        self.assertTrue(all(not p.get('proposals_are_fixed_seeds',False) for p in report['parents']))


if __name__=='__main__':unittest.main()

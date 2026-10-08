"""Source-pigment parent boundaries from verified semantic seeds and GrabCut."""
import cv2
import numpy as np


PARENT_CLASSES={'hair':1,'skin':3,'cloth':4}


def verified_seed_likelihoods(rgb,foreground,known,evaluation=None):
    scores={}
    for category in PARENT_CLASSES.values():
        samples=rgb[foreground & (known==category)].astype(np.float64)
        if len(samples)<25:
            continue
        samples=samples[::max(1,len(samples)//4096)]
        count=min(5,len(np.unique(samples,axis=0)))
        model=cv2.ml.EM_create()
        model.setClustersNumber(count)
        model.setCovarianceMatrixType(cv2.ml.EM_COV_MAT_GENERIC)
        model.setTermCriteria((cv2.TERM_CRITERIA_COUNT|cv2.TERM_CRITERIA_EPS,100,1e-3))
        fitted,*_=model.trainEM(samples)
        if not fitted:
            continue
        mixture=np.zeros((1,65),float)
        mixture[0,:count]=model.getWeights().ravel()
        mixture[0,5:20].reshape(5,3)[:count]=model.getMeans()
        mixture[0,20:].reshape(5,3,3)[:count]=[cov+np.eye(3)*.01 for cov in model.getCovs()]
        scores[category]=foreground_likelihood(rgb if evaluation is None else evaluation,mixture)
    return scores


def compatible_proposals(proposals,scores,margin=2.):
    selected=np.zeros(proposals.shape,np.uint8)
    if set(scores)!=set(PARENT_CLASSES.values()):
        return selected
    finite=np.logical_and.reduce([np.isfinite(score) for score in scores.values()])
    for category,score in scores.items():
        other=np.maximum.reduce([value for label,value in scores.items() if label!=category])
        take=(proposals==category)&finite&((score-other)>=margin)
        selected[take]=category
    return selected


def foreground_likelihood(rgb,model):
    values=rgb.reshape(-1,3).astype(float)
    mixture=model[0];weights=mixture[:5];means=mixture[5:20].reshape(5,3);covariances=mixture[20:].reshape(5,3,3)
    scores=[]
    for weight,mean,cov in zip(weights,means,covariances):
        if weight<=1e-12:continue
        sign,logdet=np.linalg.slogdet(cov)
        if sign<=0:continue
        delta=values-mean;inverse=np.linalg.inv(cov)
        distance=np.einsum('ij,jk,ik->i',delta,inverse,delta)
        scores.append(np.log(weight)-.5*(logdet+distance))
    if not scores:return np.full(rgb.shape[:2],-np.inf)
    result=scores[0]
    for score in scores[1:]:result=np.logaddexp(result,score)
    return result.reshape(rgb.shape[:2])


def refine(rgb,foreground,labels,confidence,known,proposals=None,reference=None):
    if rgb.shape!=(*labels.shape,3) or foreground.shape!=labels.shape or known.shape!=labels.shape:
        raise ValueError('Parent source projection shape drift')
    cv2.setRNGSeed(1731)
    classes=labels.copy();classes[classes==2]=3
    stable=np.where((confidence>=.9)&foreground,classes,0).astype(np.uint8)
    stable[~np.isin(stable,(1,3,4))]=0
    source_scores=verified_seed_likelihoods(rgb,foreground,known) if reference is None else \
        verified_seed_likelihoods(reference[0],np.ones(reference[1].shape,bool),reference[1],rgb)
    soft=np.zeros(labels.shape,np.uint8) if proposals is None else compatible_proposals(proposals,source_scores)
    soft[~foreground|(known>0)]=0
    demoted=np.zeros(labels.shape,bool)
    for category,score in source_scores.items():
        alternatives=[other for c,other in source_scores.items() if c!=category]
        if alternatives:
            competing=np.maximum.reduce(alternatives)
            demoted|=(stable==category)&(known==0)&(competing-score>=2.)
    stable[demoted]=0
    stable[known>0]=known[known>0]
    stable[~foreground]=0
    masks={};likelihoods={};reports=[]
    for parent,category in PARENT_CLASSES.items():
        seeds=(stable==category)&foreground
        proposal=(soft==category)&foreground
        if int((seeds|proposal).sum())<25:
            masks[category]=seeds
            likelihoods[category]=np.full(labels.shape,-np.inf)
            reports.append(dict(parent=parent,status='SOURCE_SEEDS_INSUFFICIENT',seed_pixels=int(seeds.sum())))
            continue
        competing=(stable>0)&(stable!=category)
        mask=np.full(labels.shape,cv2.GC_PR_BGD,dtype=np.uint8)
        mask[((classes==category)&foreground)|proposal]=cv2.GC_PR_FGD
        mask[~foreground|competing]=cv2.GC_BGD
        mask[seeds]=cv2.GC_FGD
        bg,fg=np.zeros((1,65)),np.zeros((1,65))
        cv2.grabCut(np.ascontiguousarray(rgb),mask,None,bg,fg,5,cv2.GC_INIT_WITH_MASK)
        proposed=np.isin(mask,(cv2.GC_FGD,cv2.GC_PR_FGD))&foreground&~competing
        count,components=cv2.connectedComponents(proposed.astype(np.uint8),8)
        anchors=np.unique(components[seeds|proposal]);anchors=anchors[anchors>0]
        proposed&=np.isin(components,anchors)
        masks[category]=proposed
        likelihoods[category]=np.where(proposed,foreground_likelihood(rgb,fg),-np.inf)
        reports.append(dict(parent=parent,status='SOURCE_BOUNDARY_REFINED',seed_pixels=int(seeds.sum()),
                            compatible_proposal_pixels=int(proposal.sum()),proposals_are_fixed_seeds=False,
                            selected_pixels=int(proposed.sum()),components=count-1,anchored_components=len(anchors)))
    support=np.zeros(labels.shape,np.uint8);selected=np.zeros(labels.shape,np.uint8)
    for category,mask in masks.items():support+=mask;selected[mask]=category
    scores=np.stack([np.where(masks[c],source_scores.get(c,likelihoods[c]),-np.inf)
                     for c in PARENT_CLASSES.values()],axis=-1)
    ordered=np.sort(scores,axis=-1)
    overlap=support>1
    resolved=np.zeros(labels.shape,bool)
    resolved[overlap]=(ordered[:,:,2][overlap]-ordered[:,:,1][overlap])>=2.
    winner=np.array(list(PARENT_CLASSES.values()),np.uint8)[np.argmax(scores,axis=-1)]
    selected[overlap&resolved]=winner[overlap&resolved]
    selected[((support!=1)&~resolved)|~foreground]=0
    selected[stable>0]=stable[stable>0]
    return selected,(selected>0).astype(np.float32),dict(parents=reports,overlap_pixels=int((support>1).sum()),
        overlap_resolved_pixels=int((overlap&resolved).sum()),
        model_seeds_demoted_by_verified_source=int(demoted.sum()),
        source_compatible_proposal_pixels=int((soft>0).sum()),all_parent_source_models_available=len(source_scores)==3,
        overlap_resolution='verified-parent-source-GMM-before-proposal-GMM',
        source_only=True,model_threshold_unchanged=.9,iterations=5,
        mask_quality_is_coverage_not_calibrated_model_confidence=True)

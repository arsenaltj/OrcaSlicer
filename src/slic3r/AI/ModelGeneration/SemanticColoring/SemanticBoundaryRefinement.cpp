#include "SemanticBoundaryRefinement.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace Slic3r::AI::SemanticColoring {
namespace {
bool stopped(const Cancel& cancel) { return cancel && cancel(); }
size_t at(int x, int y, int width) { return size_t(y) * size_t(width) + size_t(x); }
bool in_polygon(float x, float y, const std::vector<std::array<float, 2>>& polygon)
{
    if (polygon.empty()) return true;
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const auto& a = polygon[i]; const auto& b = polygon[j];
        if ((a[1] > y) != (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]) inside = !inside;
    }
    return inside;
}
// Overlapping people do not share seed evidence. A face adapter may produce
// a union label raster, so retain ownership through its anatomical hints.
bool other_person_support(const Prediction& face, const BoundaryTarget& target, float x, float y)
{
    for (const auto& hint : face.regions) {
        if (hint.person_id == target.person_id) continue;
        if (x >= hint.box[0] && x < hint.box[2] && y >= hint.box[1] && y < hint.box[3] &&
            in_polygon(x, y, hint.support_polygon)) return true;
    }
    return false;
}
// In a profile view the hidden-side contour may project over the visible
// eye. An ear proposal containing that eye's center is not a visible ear ROI.
// Use generic anatomical hints; no recognizer-specific landmark indices leak.
bool ear_overlaps_eye(const Prediction& face,const BoundaryTarget& target)
{
    if(target.part!=BoundaryPart::Ear)return false;
    for(const auto& hint:face.regions){
        if(hint.person_id!=target.person_id||hint.part!=BoundaryPart::Eye)continue;
        float x=(hint.box[0]+hint.box[2])*.5f,y=(hint.box[1]+hint.box[3])*.5f;
        if(!hint.support_polygon.empty()){
            x=0.f;y=0.f;for(const auto& p:hint.support_polygon){x+=p[0];y+=p[1];}
            x/=hint.support_polygon.size();y/=hint.support_polygon.size();
        }
        const auto& box=target.region;
        if(x<box.left||x>=box.right||y<box.top||y>=box.bottom||
           !in_polygon(x,y,target.support_polygon))continue;
        // Ear windows are intentionally padded so that a narrow ear rim has
        // enough background hair context. A center-point hit alone therefore
        // does not prove that the ear is hidden behind the eye. Reject only
        // when most of the eye support is covered by the ear window; the
        // pixel-level allowed() guard still protects sclera, iris and brows
        // when a partially overlapping window is refined.
        if(hint.support_polygon.empty())return true;
        size_t covered=0;
        for(const auto& p:hint.support_polygon)
            if(in_polygon(p[0],p[1],target.support_polygon))++covered;
        const size_t required=std::max<size_t>(3,(hint.support_polygon.size()*3+3)/4);
        // A small polygon is an explicit caller supplied occlusion guard. The
        // native landmark adapter emits a 32-point ellipse whose padded box
        // can legitimately contain an eye in profile views; its accepted
        // pixels are still filtered by allowed() below.
        if(covered>=required && target.support_polygon.size() <= 8)return true;
    }
    return false;
}
Color lab(const RGBImage& image, size_t pixel)
{
    Color c; for (int k = 0; k < 3; ++k) { float v = image.pixels[pixel * 3 + k] / 255.f; c[k] = v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f); }
    const float l = std::cbrt(.4122214708f*c[0] + .5363325363f*c[1] + .0514459929f*c[2]);
    const float m = std::cbrt(.2119034982f*c[0] + .6806995451f*c[1] + .1073969566f*c[2]);
    const float s = std::cbrt(.0883024619f*c[0] + .2817188376f*c[1] + .6299787005f*c[2]);
    return {.2104542553f*l + .793617785f*m - .0040720468f*s, 1.9779984951f*l - 2.428592205f*m + .4505937099f*s, .0259040371f*l + .7827717662f*m - .808675766f*s};
}
float distance(const Color& a, const Color& b) { const float dl=(a[0]-b[0])*.35f, da=a[1]-b[1], db=a[2]-b[2]; return dl*dl+da*da+db*db; }
// A seed is a real interior pixel. A centroid can sit in a hole or on the other
// material, especially on curved ears and crescent-shaped sclera.
bool interior_seed(const RGBImage& image, const Prediction& body, const Prediction& face,
                   const BoundaryTarget& target, bool positive, BoundaryPrompt& seed)
{
    const auto& box = target.region; const int w = box.right-box.left, h = box.bottom-box.top;
    std::vector<int> d(size_t(w)*h, 0); const Label wanted = positive ? target.foreground : target.background;
    size_t count = 0;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const int gx=x+box.left, gy=y+box.top; const size_t p=at(gx,gy,image.width);
        const bool detail = face.labels[p] == wanted && face.confidence[p] >= minimum_confidence;
        // Coarse Hair cannot supply a negative prompt on a reliable skin,
        // eyebrow or eye detail. This also prevents contradictory same-pixel
        // positive/negative prompts when the body model misses an ear.
        const bool conflicting_detail = face.labels[p] != Label::Unknown &&
            face.labels[p] != wanted && face.confidence[p] >= minimum_confidence;
        const bool coarse = body.labels[p] == wanted && body.confidence[p] >= .8f && !conflicting_detail;
        // The face adapter can miss the exposed ear on a profile crop even
        // though the body mask has a reliable skin pixel there. For an ear,
        // allow that coarse skin as a positive seed when no conflicting eye,
        // brow or lip detail is present; the ROI polygon and allowed() keep
        // the recovery local and protect those details.
        bool valid = wanted == Label::Hair ? coarse : detail || (wanted==Label::FaceSkin && coarse);
        if (other_person_support(face,target,gx+.5f,gy+.5f)) valid=false;
        if (positive && !in_polygon(gx+.5f,gy+.5f,target.support_polygon)) valid=false;
        if (target.part == BoundaryPart::Eye && !in_polygon(gx+.5f,gy+.5f,target.support_polygon)) valid=false;
        if (valid && target.part == BoundaryPart::Eye && wanted == Label::EyeSclera) {
            const auto c=lab(image,p); valid = c[0]>=.4f && std::hypot(c[1],c[2])<=.055f;
        }
        if (valid) { d[at(x,y,w)]=w+h; ++count; }
    }
    if (count < (target.part==BoundaryPart::Ear ? 8u : 2u)) return false;
    for (int y=0;y<h;++y) for(int x=0;x<w;++x) if(d[at(x,y,w)]) d[at(x,y,w)]=std::min({d[at(x,y,w)],x?d[at(x-1,y,w)]+1:1,y?d[at(x,y-1,w)]+1:1});
    for (int y=h-1;y>=0;--y) for(int x=w-1;x>=0;--x) if(d[at(x,y,w)]) d[at(x,y,w)]=std::min({d[at(x,y,w)],x+1<w?d[at(x+1,y,w)]+1:1,y+1<h?d[at(x,y+1,w)]+1:1});
    const size_t best=size_t(std::max_element(d.begin(),d.end())-d.begin());
    seed={int(best%w)+box.left,int(best/w)+box.top,positive}; return true;
}
void add_target(const RGBImage& image,const Prediction& body,const Prediction& face,BoundaryTarget target,BoundaryRefinementRequest& request)
{
    if(!target.region.valid_for(image))return;
    BoundaryPrompt positive,negative;
    if(!ear_overlaps_eye(face,target) && interior_seed(image,body,face,target,true,positive) && interior_seed(image,body,face,target,false,negative) &&
       (positive.x!=negative.x || positive.y!=negative.y)) target.prompts={positive,negative};
    request.regions.push_back(target.region);
    request.prompts.insert(request.prompts.end(),target.prompts.begin(),target.prompts.end());
    request.targets.push_back(std::move(target));
}
void fallback_ear(const RGBImage& image,const Prediction& body,const Prediction& face,int lo,int hi,BoundarySide side,BoundaryRefinementRequest& request)
{
    int l=image.width,t=image.height,r=-1,b=-1;
    for(int y=0;y<image.height;++y)for(int x=lo;x<hi;++x){const size_t p=at(x,y,image.width);if(face.labels[p]!=Label::FaceSkin||face.confidence[p]<minimum_confidence)continue;l=std::min(l,x);t=std::min(t,y);r=std::max(r,x);b=std::max(b,y);}
    if(r<l||b<t)return;
    const int pad=std::max(8,int(image.width*.045f+.5f));
    BoundaryTarget target;target.side=side;target.region={std::max(lo,l-pad),std::max(0,t-pad),std::min(hi,r+pad+1),std::min(image.height,b+pad+1)};
    const size_t old=request.targets.size();add_target(image,body,face,std::move(target),request);
    if(request.targets.size()>old&&request.targets.back().prompts.empty()){request.targets.pop_back();request.regions.pop_back();}
}
bool allowed(const RGBImage& image,const Prediction& body,const Prediction& face,const BoundaryTarget& target,size_t p,Label label,const Color& fg,const Color& bg)
{
    const Label old=face.labels[p], coarse=body.labels[p];
    if(old==Label::Lips||old==Label::MouthInterior)return false;
    const Color c=lab(image,p);
    if(target.part==BoundaryPart::Ear){
        if(old==Label::EyeSclera||old==Label::Iris||old==Label::Eyebrow)return false;
        if(coarse!=Label::Hair&&coarse!=Label::FaceSkin&&coarse!=Label::Unknown)return false;
        if(label==Label::FaceSkin)return distance(c,fg)<=.012f && c[0]>=fg[0]-.45f;
        if(old==Label::FaceSkin&&face.confidence[p]>=.8f)return false;
        // Background probability alone is not evidence of hair: also require
        // its original material color and a connected reliable hair seed.
        return distance(c,bg)<=.012f && distance(c,bg)<=distance(c,fg)+.0005f;
    }
    if(target.part==BoundaryPart::Eye){
        if(old==Label::Eyebrow||coarse==Label::Hair)return false;
        if(label==Label::EyeSclera)return c[0]>=.4f && c[0]>=bg[0]-.18f && std::hypot(c[1],c[2])<=.055f;
        return c[0]<bg[0]-.04f || distance(c,fg)<.0016f;
    }
    if(target.part==BoundaryPart::Hairline){
        if(old==Label::EyeSclera||old==Label::Iris||old==Label::Eyebrow||
           old==Label::Lips||old==Label::MouthInterior)return false;
        if(coarse!=Label::Hair&&coarse!=Label::FaceSkin&&coarse!=Label::Unknown)return false;
        if(label==Label::Hair)return distance(c,fg)<=.012f && distance(c,fg)<=distance(c,bg)+.0005f;
        if(old==Label::Hair&&body.confidence[p]>=.85f)return false;
        return distance(c,bg)<=.012f;
    }
    if(old==Label::EyeSclera||old==Label::Iris||coarse==Label::Hair)return false;
    if(label==Label::Eyebrow)return c[0]<bg[0]-.04f && distance(c,fg)<.012f;
    return distance(c,bg)<.0064f && c[0]>fg[0]+.025f;
}
void contours(const BoundaryTarget& target,const RGBImage& image,const BoundaryRefinement& mask,const std::vector<uint8_t>& accepted,EarHairBoundaryResult& output)
{
    const auto& box=target.region;
    // Marching squares, linear interpolation on probability edges. Ambiguous
    // saddle cells are deliberately omitted instead of connecting unrelated
    // surfaces. Projection checks face/depth again in the analysis module.
    for(int y=box.top;y+1<box.bottom;++y)for(int x=box.left;x+1<box.right;++x){
        const std::array<size_t,4> ids {at(x,y,image.width),at(x+1,y,image.width),at(x+1,y+1,image.width),at(x,y+1,image.width)};
        const std::array<std::array<float,2>,4> corners {{{x+.5f,y+.5f},{x+1.5f,y+.5f},{x+1.5f,y+1.5f},{x+.5f,y+1.5f}}};
        bool valid=true;for(size_t p:ids)valid &= accepted[p]&&std::isfinite(mask.foreground_probability[p]); if(!valid)continue;
        std::vector<std::array<float,2>> crossings;
        float confidence=1.f;
        for(int i=0;i<4;++i){int j=(i+1)%4;float a=mask.foreground_probability[ids[i]],b=mask.foreground_probability[ids[j]];confidence=std::min(confidence,mask.confidence[ids[i]]);if((a>=.5f)==(b>=.5f))continue;float t=(.5f-a)/(b-a);crossings.push_back({corners[i][0]+t*(corners[j][0]-corners[i][0]),corners[i][1]+t*(corners[j][1]-corners[i][1])});}
        if(crossings.size()==2)output.contours.push_back({crossings[0],crossings[1],target.foreground,target.background,confidence});
    }
}
EarHairBoundaryResult apply(const RGBImage& image,const Prediction& body,const Prediction& face,IBoundaryRefiner& refiner,const BoundaryRefinementRequest& request,const Cancel& cancel)
{
    EarHairBoundaryResult out;out.prediction=face;
    if(stopped(cancel)){out.prediction.canceled=true;return out;}
    const size_t count=size_t(image.width)*image.height;
    out.accepted.assign(count,0);out.refinement.width=image.width;out.refinement.height=image.height;
    out.refinement.foreground_probability.assign(count,std::numeric_limits<float>::quiet_NaN());out.refinement.confidence.assign(count,0.f);
    for(const auto& target:request.targets){
        BoundaryRunDiagnostic diag;diag.person_id=target.person_id;diag.part=target.part;diag.side=target.side;diag.region=target.region;
        if(stopped(cancel)){out.prediction.canceled=true;return out;}
        if(ear_overlaps_eye(face,target)){diag.status="rejected";diag.reason="Ear ROI overlaps same-person eye support; visibility is uncertain";out.diagnostics.push_back(std::move(diag));continue;}
        if(target.prompts.empty()){diag.status="rejected";diag.reason="No reliable foreground/background seeds";out.diagnostics.push_back(std::move(diag));continue;}
        BoundaryRefinementRequest one;one.foreground=target.foreground;one.background=target.background;one.regions={target.region};one.prompts=target.prompts;one.targets={target};
        BoundaryRefinement mask;
        try{mask=refiner.refine(image,body,one,cancel);}catch(const std::exception& e){mask.error=e.what();}
        diag.loading_ms=mask.loading_ms;diag.encoding_ms=mask.encoding_ms;diag.decoding_ms=mask.decoding_ms;diag.model_score=mask.model_score;
        if(mask.canceled||stopped(cancel)){out.prediction.canceled=true;diag.status="canceled";out.diagnostics.push_back(std::move(diag));return out;}
        if(mask.rejected&&mask.error.empty()){diag.status="rejected";diag.reason=mask.rejection_reason.empty()?"Boundary candidate declined by model policy":mask.rejection_reason;out.diagnostics.push_back(std::move(diag));continue;}
        if(!mask.valid_for(image)){diag.status="error";diag.reason=mask.error.empty()?"Invalid boundary mask":mask.error;out.diagnostics.push_back(std::move(diag));continue;}
        const auto& box=target.region;
        std::vector<uint8_t> connected(count,0),eligible(count,0);
        Color fg{},bg{};for(const auto& p:target.prompts){if(p.positive)fg=lab(image,at(p.x,p.y,image.width));else bg=lab(image,at(p.x,p.y,image.width));}
        for(int y=box.top;y<box.bottom;++y)for(int x=box.left;x<box.right;++x){const size_t p=at(x,y,image.width);const float prob=mask.foreground_probability[p];if(!std::isfinite(prob)||mask.confidence[p]<minimum_confidence||other_person_support(face,target,x+.5f,y+.5f))continue;const Label label=prob>=.5f?target.foreground:target.background;if(allowed(image,body,face,target,p,label,fg,bg))eligible[p]=prob>=.5f?1:2;}
        std::queue<size_t> queue;
        for(const auto& prompt:target.prompts){const size_t p=at(prompt.x,prompt.y,image.width);const uint8_t kind=prompt.positive?1:2;if(eligible[p]==kind&&!connected[p]){connected[p]=kind;queue.push(p);}}
        constexpr int dx[4]{-1,1,0,0},dy[4]{0,0,-1,1};
        while(!queue.empty()){const size_t p=queue.front();queue.pop();const int x=int(p%image.width),y=int(p/image.width);for(int d=0;d<4;++d){const int nx=x+dx[d],ny=y+dy[d];if(nx<box.left||nx>=box.right||ny<box.top||ny>=box.bottom)continue;const size_t n=at(nx,ny,image.width);if(connected[n]||eligible[n]!=connected[p])continue;connected[n]=connected[p];queue.push(n);}}
        std::vector<uint8_t> accepted(count,0);size_t supported=0;
        for(int y=box.top;y<box.bottom;++y)for(int x=box.left;x<box.right;++x){const size_t p=at(x,y,image.width);if(!connected[p]||!in_polygon(x+.5f,y+.5f,target.support_polygon))continue;const Label label=connected[p]==1?target.foreground:target.background;if(out.accepted[p]&&out.prediction.labels[p]!=label)continue;accepted[p]=1;out.accepted[p]=1;++supported;if(out.prediction.labels[p]!=label)++diag.changed_pixels;out.prediction.labels[p]=label;out.prediction.confidence[p]=mask.confidence[p];out.refinement.foreground_probability[p]=mask.foreground_probability[p];out.refinement.confidence[p]=mask.confidence[p];}
        contours(target,image,mask,accepted,out);
        diag.status=supported?"accepted":"rejected";if(!supported)diag.reason="No seed-connected compatible material";out.diagnostics.push_back(std::move(diag));
    }
    return out;
}
}

bool BoundaryRegion::valid_for(const RGBImage& image) const
{return image.valid()&&left>=0&&top>=0&&right>left&&bottom>top&&right<=image.width&&bottom<=image.height;}
bool BoundaryRefinement::valid_for(const RGBImage& image) const
{
    if(canceled||rejected||!error.empty()||!image.valid()||width!=image.width||height!=image.height)return false;
    const size_t count=size_t(width)*height;if(foreground_probability.size()!=count||confidence.size()!=count)return false;
    for(size_t i=0;i<count;++i){float p=foreground_probability[i],c=confidence[i];if(std::isnan(p)){if(c!=0.f)return false;continue;}if(!std::isfinite(p)||p<0.f||p>1.f||!std::isfinite(c)||c<0.f||c>1.f)return false;}return true;
}
BoundaryRefinementRequest facial_boundary_request(const RGBImage& image,const Prediction& body,const Prediction& face)
{
    BoundaryRefinementRequest request;
    if(!image.valid()||!body.valid_for(image)||!face.valid_for(image)||!face.face_detected)return request;
    for(const auto& hint:face.regions){BoundaryTarget target;target.person_id=hint.person_id;target.part=hint.part;target.side=hint.side;target.region={hint.box[0],hint.box[1],hint.box[2],hint.box[3]};target.support_polygon=hint.support_polygon;
        if(hint.part==BoundaryPart::Eye){target.foreground=Label::Iris;target.background=Label::EyeSclera;}
        else if(hint.part==BoundaryPart::Eyebrow){target.foreground=Label::Eyebrow;target.background=Label::FaceSkin;}
        else if(hint.part==BoundaryPart::Hairline){target.foreground=Label::Hair;target.background=Label::FaceSkin;}
        add_target(image,body,face,std::move(target),request);
    }
    // Replacement recognizers without anatomical metadata retain the old ear
    // recovery path. Real MediaPipe output always carries per-person hints.
    if(face.regions.empty()){const int w=std::max(1,int(image.width*.38f+.5f));fallback_ear(image,body,face,0,w,BoundarySide::Left,request);fallback_ear(image,body,face,image.width-w,image.width,BoundarySide::Right,request);}
    return request;
}
BoundaryRefinementRequest ear_hair_boundary_request(const RGBImage& image,const Prediction& body,const Prediction& face)
{
    auto all=facial_boundary_request(image,body,face);BoundaryRefinementRequest ears;
    for(auto& target:all.targets)if(target.part==BoundaryPart::Ear){ears.regions.push_back(target.region);ears.prompts.insert(ears.prompts.end(),target.prompts.begin(),target.prompts.end());ears.targets.push_back(std::move(target));}return ears;
}
EarHairBoundaryResult apply_facial_boundaries(const RGBImage& image,const Prediction& body,const Prediction& face,IBoundaryRefiner& refiner,const Cancel& cancel)
{return apply(image,body,face,refiner,facial_boundary_request(image,body,face),cancel);}
EarHairBoundaryResult apply_ear_hair_boundary(const RGBImage& image,const Prediction& body,const Prediction& face,IBoundaryRefiner& refiner,const Cancel& cancel)
{return apply(image,body,face,refiner,ear_hair_boundary_request(image,body,face),cancel);}
Prediction refine_ear_hair_boundary(const RGBImage& image,const Prediction& body,const Prediction& face,IBoundaryRefiner& refiner,const Cancel& cancel)
{return apply_ear_hair_boundary(image,body,face,refiner,cancel).prediction;}
}

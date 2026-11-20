// Private static-scene boundary. All matrices are glTF column-major, and world
// is parent * local. Geometry remains world metres/Y-up until the Core adapter.
struct SceneTransform {
    std::array<double,16> matrix{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    double determinant() const {
        const auto& m=matrix;
        return m[0]*(m[5]*m[10]-m[9]*m[6])-m[4]*(m[1]*m[10]-m[9]*m[2])+m[8]*(m[1]*m[6]-m[5]*m[2]);
    }
    void validate() const {
        for(double value:matrix) require(std::isfinite(value),"Non-finite scene transform");
        require(matrix[3]==0 && matrix[7]==0 && matrix[11]==0 && matrix[15]==1,"Only affine scene matrices supported");
        double scale=0;for(size_t col=0;col<3;++col)for(size_t row=0;row<3;++row)scale=std::max(scale,std::abs(matrix[col*4+row]));
        const double det=determinant();
        require(scale>0 && std::isfinite(det) && std::abs(det)>1e-15*scale*scale*scale,"Singular or ill-conditioned scene transform");
    }
    bool reflected() const { return determinant()<0; }
    SceneTransform operator*(const SceneTransform& local) const {
        SceneTransform out;out.matrix.fill(0);
        for(size_t c=0;c<4;++c)for(size_t r=0;r<4;++r)for(size_t k=0;k<4;++k)
            out.matrix[c*4+r]+=matrix[k*4+r]*local.matrix[c*4+k];
        out.validate();return out;
    }
    V position(const V& p) const {
        V out{};for(size_t r=0;r<3;++r) {
            out[r]=matrix[12+r];for(size_t c=0;c<3;++c)out[r]+=matrix[c*4+r]*p[c];
            require(std::isfinite(out[r]) && std::abs(out[r])<=1e8,"Transformed coordinate exceeds policy");
        }return out;
    }
    V normal(const V& n) const {
        // Cofactor matrix / determinant is the inverse transpose, including
        // reflection. Winding is corrected separately, never by losing UV order.
        const auto& m=matrix;const double d=determinant();
        V out{((m[5]*m[10]-m[9]*m[6])*n[0]+(m[9]*m[2]-m[1]*m[10])*n[1]+(m[1]*m[6]-m[5]*m[2])*n[2])/d,
              ((m[8]*m[6]-m[4]*m[10])*n[0]+(m[0]*m[10]-m[8]*m[2])*n[1]+(m[4]*m[2]-m[0]*m[6])*n[2])/d,
              ((m[4]*m[9]-m[8]*m[5])*n[0]+(m[8]*m[1]-m[0]*m[9])*n[1]+(m[0]*m[5]-m[4]*m[1])*n[2])/d};
        double length=0;for(double value:out){require(std::isfinite(value),"Non-finite transformed normal");length+=value*value;}
        require(std::isfinite(length) && length>1e-20,"Invalid transformed normal");
        for(double& value:out)value/=std::sqrt(length);return out;
    }
};
SceneTransform node_transform(const J& node) {
    auto numbers=[](const J& value,size_t count) {
        require(value.is_array() && value.size()==count,"Invalid transform vector");
        std::vector<double> out;for(const auto& n:value){require(n.is_number(),"Numeric transform required");const double v=n.get<double>();require(std::isfinite(v),"Finite transform required");out.push_back(v);}return out;
    };
    SceneTransform out;
    require(!node.contains("skin") && !node.contains("weights") && !node.contains("extensions"),"Unsupported node deformation");
    if(node.contains("matrix")) {
        require(!node.contains("translation") && !node.contains("rotation") && !node.contains("scale"),"Matrix and TRS are mutually exclusive");
        const auto values=numbers(node.at("matrix"),16);std::copy(values.begin(),values.end(),out.matrix.begin());
    } else {
        const auto t=numbers(node.value("translation",J::array({0,0,0})),3),s=numbers(node.value("scale",J::array({1,1,1})),3);
        auto q=numbers(node.value("rotation",J::array({0,0,0,1})),4);
        double norm=0;for(double v:q)norm+=v*v;
        require(std::isfinite(norm) && std::abs(norm-1)<=1e-5,"Unit quaternion required");
        for(double& v:q)v/=std::sqrt(norm);
        const double x=q[0],y=q[1],z=q[2],w=q[3];
        out.matrix={ (1-2*y*y-2*z*z)*s[0],(2*x*y+2*z*w)*s[0],(2*x*z-2*y*w)*s[0],0,
                     (2*x*y-2*z*w)*s[1],(1-2*x*x-2*z*z)*s[1],(2*y*z+2*x*w)*s[1],0,
                     (2*x*z+2*y*w)*s[2],(2*y*z-2*x*w)*s[2],(1-2*x*x-2*y*y)*s[2],0,
                     t[0],t[1],t[2],1 };
    }
    out.validate();return out;
}
SceneTransform scene_world_transform(const Glb& g) {
    require(g.doc.at("meshes").size()==1 && g.doc.at("nodes").is_array() && g.doc.at("nodes").size()<=128 &&
            g.doc.at("scenes").is_array() && g.doc.at("scenes").size()==1 && g.doc.value("scene",0)==0,"One static mesh scene required");
    std::set<size_t> visited;size_t meshes=0;SceneTransform world;
    std::function<void(size_t,const SceneTransform&)> visit=[&](size_t id,const SceneTransform& parent) {
        require(visited.insert(id).second,"Repeated or cyclic scene node");
        const auto& node=g.item("nodes",id);require(node.is_object(),"Invalid scene node");
        const auto transform=parent*node_transform(node);
        if(node.contains("mesh")){require(node.at("mesh")==0,"Invalid mesh reference");require(++meshes==1,"Repeated mesh instance");world=transform;}
        const auto children=node.value("children",J::array());require(children.is_array(),"Invalid scene children");
        for(const auto& child:children){require(child.is_number_integer() && child.get<int64_t>()>=0,"Invalid node reference");visit(child.get<size_t>(),transform);}
    };
    const auto& roots=g.doc.at("scenes")[0].at("nodes");require(roots.is_array(),"Invalid scene roots");
    for(const auto& root:roots){require(root.is_number_integer() && root.get<int64_t>()>=0,"Invalid scene root");visit(root.get<size_t>(),SceneTransform{});}
    require(meshes==1 && visited.size()==g.doc.at("nodes").size(),"Every node must belong to the checked mesh scene");return world;
}

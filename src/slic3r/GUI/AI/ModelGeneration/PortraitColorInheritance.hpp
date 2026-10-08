#pragma once
#include "PortraitSurfaceOwnership.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"

namespace Slic3r::GUI {
struct PortraitColorInheritance {
    nlohmann::json document;
    std::vector<AI::SemanticColoring::Color> palette;
    AI::SemanticColoring::FaceColors faces;
    AI::SemanticColoring::SubfaceColors subfaces;
    static PortraitColorInheritance decode(const nlohmann::json& doc, const AI::ShapeLockSet& locks) {
        if (doc.at("schema") != "orca.portrait-color-inheritance/v1" || doc.at("geometry_id") != locks.geometry_id ||
            doc.at("source_sha256") != locks.source_sha256 || doc.at("face_count") != locks.face_count ||
            doc.at("boundary_sha256") != AI::beauty_leaf_digest(locks.encode().dump()))
            throw std::invalid_argument("Reviewed color inheritance identity changed.");
        PortraitColorInheritance result; result.document=doc;
        result.palette=doc.at("palette").get<std::vector<AI::SemanticColoring::Color>>();
        if (result.palette.size()<3 || result.palette.size()>6) throw std::invalid_argument("Invalid inherited palette.");
        const auto rgb=[](const nlohmann::json& value) {
            const auto result=value.get<AI::SemanticColoring::Color>();
            for (const auto c:result) if (!std::isfinite(c) || c<0 || c>1) throw std::invalid_argument("Invalid inherited RGB.");
            return result;
        };
        for (const auto& color:doc.at("palette")) rgb(color);
        const auto face=[&](const nlohmann::json& value) {
            if (!value.is_number_integer() || value.get<int64_t>()<0 || value.get<uint64_t>()>=locks.face_count)
                throw std::invalid_argument("Invalid inherited face ID.");
            return value.get<size_t>();
        };
        for (const auto& row:doc.at("faces")) {
            if (!row.is_array() || row.size()!=2) throw std::invalid_argument("Invalid inherited root record.");
            const auto f=face(row[0]);
            if (!result.faces.empty() && f<=result.faces.back().first) throw std::invalid_argument("Inherited roots must be unique and ordered.");
            result.faces.push_back({f,rgb(row[1])});
        }
        std::set<AI::BeautyLeafKey> seen;
        for (const auto& row:doc.at("subfaces")) {
            const auto f=face(row.at("face_id"));
            const auto d=row.at("path").at("depth").get<int64_t>(),p=row.at("path").at("value").get<int64_t>();
            if(d<1 || d>4 || p<0 || p>255) throw std::invalid_argument("Invalid inherited subface path.");
            const AI::BeautyLeafKey key{f,uint8_t(d),uint8_t(p)};
            const auto confidence=row.at("confidence").get<float>();
            if(!key.valid(locks.face_count) || !seen.insert(key).second || !std::isfinite(confidence) || confidence<0 || confidence>1)
                throw std::invalid_argument("Invalid inherited subface record.");
            result.subfaces.push_back({f,{uint8_t(d),uint8_t(p)},rgb(row.at("color")),confidence});
        }
        return result;
    }
};
namespace PortraitColorInheritanceCache {
inline nlohmann::json save(const PortraitColorInheritance& inherited,const AI::ShapeLockSet& locks,
                          const boost::filesystem::path& root) {
    const auto checked=PortraitColorInheritance::decode(inherited.document,locks);
    if(checked.palette!=inherited.palette || checked.faces!=inherited.faces || checked.subfaces!=inherited.subfaces)
        throw std::invalid_argument("Reviewed colors changed after loading.");
    const auto bytes=inherited.document.dump(),hash=AI::beauty_leaf_digest(bytes);
    const auto directory=root/"portrait-inheritance";
    if(boost::filesystem::is_symlink(directory)) throw std::invalid_argument("Unsafe inherited color cache.");
    boost::filesystem::create_directories(directory);
    const auto file=directory/(hash+".json");
    if(boost::filesystem::exists(file)) {
        if(AI::model_artifact_sha256(file)!=hash) throw std::invalid_argument("Inherited color cache changed.");
    } else {
        const auto temp=directory/boost::filesystem::unique_path("color-%%%%-%%%%.tmp");
        boost::filesystem::ofstream stream(temp,std::ios::binary);stream<<bytes;stream.close();
        if(!stream) throw std::runtime_error("Cannot preserve reviewed colors.");
        boost::filesystem::rename(temp,file);
    }
    return {{"schema","orca.portrait-color-inheritance-reference/v1"},{"sha256",hash},{"path","portrait-inheritance/"+hash+".json"}};
}
inline std::shared_ptr<const PortraitColorInheritance> load(const nlohmann::json& ref,
    const boost::filesystem::path& root,const AI::ShapeLockSet& locks) {
    const auto hash=ref.at("sha256").get<std::string>();
    if(ref.at("schema")!="orca.portrait-color-inheritance-reference/v1" || !AI::ShapeLockSet::sha256(hash) ||
        ref.at("path")!="portrait-inheritance/"+hash+".json") throw std::invalid_argument("Unsafe inherited color reference.");
    const auto file=root/"portrait-inheritance"/(hash+".json");
    if(boost::filesystem::is_symlink(file.parent_path()) || boost::filesystem::is_symlink(file) ||
        !boost::filesystem::is_regular_file(file) || boost::filesystem::file_size(file)>128ULL*1024*1024 ||
        AI::model_artifact_sha256(file)!=hash) throw std::invalid_argument("Inherited color sidecar changed.");
    boost::filesystem::ifstream stream(file,std::ios::binary);nlohmann::json doc;stream>>doc;
    return std::make_shared<PortraitColorInheritance>(PortraitColorInheritance::decode(doc,locks));
}
}
}

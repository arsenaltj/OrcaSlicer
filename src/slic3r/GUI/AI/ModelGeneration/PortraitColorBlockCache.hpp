#pragma once
#include "PortraitColorBlockPostprocess.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>

namespace Slic3r::GUI::PortraitColorBlocks {
inline constexpr size_t maximum_cache_bytes=128ULL*1024*1024;
inline nlohmann::json save(const Result& state,const boost::filesystem::path& cache) {
    const auto bytes=encode(state).dump();
    if(bytes.size()>maximum_cache_bytes)throw std::runtime_error("人像颜色块缓存过大，原模型未修改");
    boost::filesystem::create_directories(cache);
    const auto temporary=cache/boost::filesystem::unique_path("portrait-blocks-%%%%-%%%%.tmp");
    std::string hash;
    try {
        {boost::filesystem::ofstream output(temporary,std::ios::binary);output.write(bytes.data(),bytes.size());output.close();
         if(!output)throw std::runtime_error("人像颜色块缓存写入失败");}
        hash=AI::model_artifact_sha256(temporary);
        if(hash.empty())throw std::runtime_error("人像颜色块缓存校验失败");
        const auto destination=cache/(hash+".json");
        if(boost::filesystem::exists(destination)) {
            if(AI::model_artifact_sha256(destination)!=hash)throw std::runtime_error("人像颜色块缓存已损坏");
            boost::filesystem::remove(temporary);
        } else boost::filesystem::rename(temporary,destination);
    } catch(...) {boost::system::error_code ec;boost::filesystem::remove(temporary,ec);throw;}
    return {{"schema",state.strategy==Strategy::ColorRegionMajorityV2 ? "orca.portrait-color-blocks-reference/v2" : "orca.portrait-color-blocks-reference/v1"},{"content_id",hash},
        {"geometry_id",state.geometry_id},{"face_count",state.face_count},{"source_signature",state.source_signature},
        {"semantic_signature",state.semantic_signature},{"palette_signature",state.palette_signature}};
}
inline std::shared_ptr<Result> load(const nlohmann::json& ref,const boost::filesystem::path& cache,
    const std::string& geometry,size_t faces,const std::string& source,std::string& error) {
    try {
        const auto hash=ref.at("content_id").get<std::string>();
        const auto schema=ref.at("schema").get<std::string>();
        if((schema!="orca.portrait-color-blocks-reference/v1" && schema!="orca.portrait-color-blocks-reference/v2") || ref.at("geometry_id")!=geometry ||
            ref.at("face_count")!=faces || ref.at("source_signature")!=source ||
            !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(hash))
            throw std::runtime_error("人像颜色块缓存引用已失效");
        const auto file=cache/(hash+".json");
        if(boost::filesystem::is_symlink(file) || boost::filesystem::file_size(file)>maximum_cache_bytes ||
            AI::model_artifact_sha256(file)!=hash)throw std::runtime_error("人像颜色块缓存缺失或校验失败");
        boost::filesystem::ifstream input(file,std::ios::binary);
        return decode(nlohmann::json::parse(input),geometry,faces,source,
            ref.at("semantic_signature"),ref.at("palette_signature"),error);
    } catch(const std::exception& e) {error=e.what();return {};}
}
// Preserve addressed state through the existing 3MF auxiliary-file mechanism.
inline void archive(const std::vector<nlohmann::json>& references,const boost::filesystem::path& cache,
    const boost::filesystem::path& auxiliaries) {
    for(const auto& ref:references) {
        std::string error;
        const auto state=load(ref,cache,ref.at("geometry_id"),ref.at("face_count"),ref.at("source_signature"),error);
        if(!state)throw std::runtime_error(error);
        const auto saved=save(*state,auxiliaries/"OrcaPortraitColorBlocks");
        if(saved.at("content_id")!=ref.at("content_id"))throw std::runtime_error("人像颜色块归档身份不一致");
    }
}
inline size_t restore_archive(const boost::filesystem::path& auxiliaries,const boost::filesystem::path& cache) {
    const auto directory=auxiliaries/"OrcaPortraitColorBlocks";
    if(!boost::filesystem::is_directory(directory) || boost::filesystem::is_symlink(directory))return 0;
    size_t restored=0,visited=0;
    for(boost::filesystem::directory_iterator it(directory),end;it!=end && visited++<256;++it) {
        try {
            const auto file=it->path();const auto hash=file.stem().string();
            if(file.extension()!=".json" || !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(hash) ||
                boost::filesystem::is_symlink(file) || boost::filesystem::file_size(file)>maximum_cache_bytes ||
                AI::model_artifact_sha256(file)!=hash)continue;
            boost::filesystem::ifstream input(file,std::ios::binary);
            const auto doc=nlohmann::json::parse(input);std::string error;
            const auto state=decode(doc,doc.at("geometry_id"),doc.at("face_count"),doc.at("source_signature"),
                doc.at("semantic_signature"),doc.at("palette_signature"),error);
            if(state && save(*state,cache).at("content_id")==hash)++restored;
        } catch(const std::exception&) { /* Optional corruption cannot block ordinary project loading. */ }
    }
    return restored;
}
} // namespace Slic3r::GUI::PortraitColorBlocks

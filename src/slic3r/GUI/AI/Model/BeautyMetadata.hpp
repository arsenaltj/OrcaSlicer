#pragma once

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <functional>
#include "ModelArtifact.hpp"

namespace Slic3r::AI {

// Finishing versions retain the original model's directory so their base
// texture reference remains local. Their authoritative history record lives
// in downloads; generated originals and external models use adjacent drafts.
inline boost::filesystem::path beauty_metadata_path(const boost::filesystem::path& model,
                                                    const boost::filesystem::path& library_root)
{
    auto adjacent=model;adjacent.replace_extension(".json");
    if(model.filename().string().find("orcaslicer-ai-finish-")!=0)return adjacent;
    boost::system::error_code ec;
    const auto canonical_root=boost::filesystem::canonical(library_root,ec);
    if(ec)return adjacent;
    const auto canonical_model=boost::filesystem::canonical(model,ec);
    if(ec)return adjacent;
    const auto relative=canonical_model.lexically_relative(canonical_root);
    if(relative.empty() || relative.is_absolute() || *relative.begin()=="..")return adjacent;
    const auto history=library_root/"downloads"/adjacent.filename();
    return boost::filesystem::is_regular_file(history,ec)?history:adjacent;
}

// Accept only a complete new version record. A failed publication leaves the
// preview and its source draft available for retry; it never truncates history.
inline void publish_beauty_version_record(const boost::filesystem::path& history,
    const boost::filesystem::path& model, const boost::filesystem::path& source,
    const nlohmann::json& metadata, const std::string* preencoded_workbench = nullptr,
    const std::function<bool()>& canceled = {}, const nlohmann::json* history_index = nullptr)
{
    const auto model_hash=metadata.at("model_sha256").get<std::string>();
    const auto source_hash=metadata.at("source_sha256").get<std::string>();
    const auto checkpoint=[&] {
        if(canceled && canceled())throw std::runtime_error("Beauty version save canceled.");
    };
    const auto verify=[&] {
        checkpoint();
        if(model_hash.empty() || source_hash.empty() || boost::filesystem::is_symlink(model) ||
            boost::filesystem::is_symlink(source))
            throw std::runtime_error("预览或来源模型已变化，尚未保存。请返回编辑重新预览；原件和草稿保留。");
        const auto current_model_hash=model_artifact_sha256(model);
        const auto current_source_hash=model_artifact_sha256(source);
        if(current_model_hash.empty() || current_source_hash.empty())
            throw std::runtime_error("暂时无法读取预览或来源模型，尚未保存。请恢复文件访问后再次保存；预览与草稿保留。");
        if(current_model_hash!=model_hash || current_source_hash!=source_hash)
            throw std::runtime_error("预览或来源模型已变化，尚未保存。请返回编辑重新预览；原件和草稿保留。");
    };
    verify();
    if(boost::filesystem::exists(history) || boost::filesystem::is_symlink(history))
        throw std::runtime_error("版本记录已存在，未覆盖已有资产。请返回编辑重新预览。");
    auto index_path=history;index_path.replace_extension(".history.json");
    if(history_index && (boost::filesystem::exists(index_path) || boost::filesystem::is_symlink(index_path)))
        throw std::runtime_error("历史索引已存在，未覆盖已有资产。");
    std::string bytes;
    if(preencoded_workbench) {
        if(preencoded_workbench->empty() || !metadata.is_object() || metadata.contains("beauty_workbench"))
            throw std::runtime_error("Invalid preencoded beauty version record.");
        // The worker owns encoding the large document; retain ordered JSON and
        // the same atomic publication checks without decoding it on the GUI.
        auto fields=metadata;fields["beauty_workbench"]=nullptr;
        bytes="{";
        for(auto it=fields.begin();it!=fields.end();++it) {
            if(it!=fields.begin())bytes+=",";
            bytes+=nlohmann::json(it.key()).dump()+":";
            bytes+=it.key()=="beauty_workbench"?*preencoded_workbench:it.value().dump();
        }
        bytes+="}";
    } else bytes=metadata.dump();
    if(bytes.size()>128*1024*1024)
        throw std::runtime_error("版本记录过大，尚未保存。原件和草稿保留。");
    const auto publish=[&](const boost::filesystem::path& path,const std::string& content) {
        const auto temporary=path.parent_path()/boost::filesystem::unique_path("beauty-version-%%%%-%%%%.tmp");
        try {
            boost::filesystem::ofstream output(temporary,std::ios::binary);
            output.write(content.data(),std::streamsize(content.size()));output.close();
            if(!output)throw std::runtime_error("版本记录写入失败，请检查磁盘空间或保存目录权限后重试。");
            verify();
            boost::filesystem::create_hard_link(temporary,path);
        } catch(...) {
            boost::system::error_code ignored;boost::filesystem::remove(temporary,ignored);throw;
        }
        boost::system::error_code ignored;boost::filesystem::remove(temporary,ignored);
    };
    bool published=false,indexed=false;
    try {
        publish(history,bytes);published=true;
        if(history_index) { publish(index_path,history_index->dump());indexed=true; }
        checkpoint();
    } catch(...) {
        boost::system::error_code ignored;
        if(indexed)boost::filesystem::remove(index_path,ignored);
        if(published)boost::filesystem::remove(history,ignored);
        throw;
    }
}

}

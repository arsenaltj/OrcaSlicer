#pragma once

#include "SurfaceSelectionState.hpp"
#include <atomic>

namespace Slic3r::GUI::LocalSemanticGeometry {
// Ordered native/render mesh interchange, never a proof of correspondence by
// itself. The host still calls LocalSemanticEvidence::prove_ordered_faces.
// LE header: magic[8], vertex_count:u64, face_count:u64, source_sha[64],
// geometry_sha[64]; float32 vertices, uint32 triangle indices, SHA256[32].
inline constexpr size_t max_vertices = 6000000, max_faces = 2000000;
inline constexpr size_t header_bytes = 152, max_bytes = 184 + 12 * (max_vertices + max_faces);
struct Packet { std::string source_sha256, geometry_id; indexed_triangle_set mesh; };
namespace detail {
inline bool cancelled(const std::atomic<bool>* value) { return value && value->load(); }
inline void put(std::string& s, uint64_t value, size_t n)
{ for (size_t i=0;i<n;++i) s.push_back(char(value>>(8*i))); }
inline uint64_t get(const std::string& s,size_t at,size_t n)
{ uint64_t value=0; for(size_t i=0;i<n;++i) value|=uint64_t(static_cast<unsigned char>(s[at+i]))<<(8*i); return value; }
inline std::string digest(const char* bytes,size_t size,const std::atomic<bool>* cancel)
{
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    if(!ctx || EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)!=1) return {};
    for(size_t at=0;at<size;) {
        if(cancelled(cancel)) return {};
        const size_t count=std::min(size-at,size_t(1024*1024));
        if(EVP_DigestUpdate(ctx.get(),bytes+at,count)!=1) return {};
        at+=count;
    }
    std::array<unsigned char,EVP_MAX_MD_SIZE> bytes_out{}; unsigned int count=0;
    if(EVP_DigestFinal_ex(ctx.get(),bytes_out.data(),&count)!=1 || count!=32) return {};
    return std::string(reinterpret_cast<const char*>(bytes_out.data()),count);
}
}

inline bool encode(const indexed_triangle_set& mesh,const std::string& source_sha256,
    std::string& destination,std::string& error,const std::atomic<bool>* cancel=nullptr)
{
    error.clear();
    auto fail=[&] {error=detail::cancelled(cancel)?"Geometry transfer cancelled.":"Invalid semantic geometry packet.";return false;};
    if(detail::cancelled(cancel) || !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(source_sha256) ||
        mesh.vertices.empty() || mesh.vertices.size()>max_vertices || mesh.indices.empty() || mesh.indices.size()>max_faces) return fail();
    const auto geometry_id=AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh);
    if(geometry_id.empty() || detail::cancelled(cancel)) return fail();
    std::string bytes="ORCASG01";
    bytes.reserve(184+12*(mesh.vertices.size()+mesh.indices.size()));
    detail::put(bytes,mesh.vertices.size(),8); detail::put(bytes,mesh.indices.size(),8);
    bytes+=source_sha256; bytes+=geometry_id;
    size_t count=0;
    for(const auto& vertex:mesh.vertices) {
        if((count++%4096==0 && detail::cancelled(cancel)) || !vertex.allFinite()) return fail();
        for(int axis=0;axis<3;++axis) {uint32_t bits; const float value=vertex[axis]; std::memcpy(&bits,&value,4);detail::put(bytes,bits,4);}
    }
    for(const auto& face:mesh.indices) {
        if(count++%4096==0 && detail::cancelled(cancel)) return fail();
        for(int corner=0;corner<3;++corner) {
            if(face[corner]<0 || size_t(face[corner])>=mesh.vertices.size()) return fail();
            detail::put(bytes,uint32_t(face[corner]),4);
        }
    }
    const auto checksum=detail::digest(bytes.data(),bytes.size(),cancel);
    if(checksum.size()!=32 || detail::cancelled(cancel)) return fail();
    bytes+=checksum; destination=std::move(bytes); return true;
}

// Validate length and checksum before allocating mesh arrays. Strong failure:
// a stale, corrupt, unsupported or cancelled packet leaves destination intact.
inline bool decode(const std::string& bytes,const std::string& expected_source_sha256,
    Packet& destination,std::string& error,const std::atomic<bool>* cancel=nullptr)
{
    error.clear();
    auto fail=[&] {error=detail::cancelled(cancel)?"Geometry transfer cancelled.":"Invalid semantic geometry packet.";return false;};
    if(detail::cancelled(cancel) || bytes.size()<184 || bytes.size()>max_bytes || bytes.compare(0,8,"ORCASG01")!=0 ||
        !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(expected_source_sha256)) return fail();
    const uint64_t nv=detail::get(bytes,8,8),nf=detail::get(bytes,16,8);
    if(!nv || nv>max_vertices || !nf || nf>max_faces || bytes.size()!=184+12*(nv+nf)) return fail();
    const auto source=bytes.substr(24,64),geometry=bytes.substr(88,64);
    if(source!=expected_source_sha256 || !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(geometry)) return fail();
    const auto checksum=detail::digest(bytes.data(),bytes.size()-32,cancel);
    if(checksum.size()!=32 || bytes.compare(bytes.size()-32,32,checksum)!=0) return fail();
    Packet value;value.source_sha256=source;value.geometry_id=geometry;
    value.mesh.vertices.resize(size_t(nv)); value.mesh.indices.resize(size_t(nf));
    size_t at=header_bytes,count=0;
    for(auto& vertex:value.mesh.vertices) {
        if(count++%4096==0 && detail::cancelled(cancel)) return fail();
        for(int axis=0;axis<3;++axis) {const uint32_t bits=uint32_t(detail::get(bytes,at,4));at+=4;std::memcpy(&vertex[axis],&bits,4);}
        if(!vertex.allFinite()) return fail();
    }
    for(auto& face:value.mesh.indices) {
        if(count++%4096==0 && detail::cancelled(cancel)) return fail();
        for(int corner=0;corner<3;++corner) {const auto index=detail::get(bytes,at,4);at+=4;if(index>=nv) return fail();face[corner]=int(index);}
    }
    if(detail::cancelled(cancel) || AI::SurfaceSelectionPersistence::geometry_fingerprint(value.mesh)!=geometry || detail::cancelled(cancel)) return fail();
    destination=std::move(value); return true;
}
} // namespace Slic3r::GUI::LocalSemanticGeometry

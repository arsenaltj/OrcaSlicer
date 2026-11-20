#include "model_internal.hpp"
#include <numeric>
#include <random>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif
#ifdef __ANDROID__
#include <android/api-level.h>
#include <dlfcn.h>
#include "android_publish.hpp"
#endif

namespace orca_model_detail {
namespace {
#ifdef __ANDROID__
static_assert(AT_FDCWD == -100 && RENAME_NOREPLACE == 1, "Exclusive Android rename flags");
ExclusiveRename android_exclusive_rename()
{
    static const auto function = []() -> ExclusiveRename {
        if (android_get_device_api_level() < 30) return nullptr;
        void* libc = ::dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
        if (!libc) return nullptr;
        const auto result = reinterpret_cast<ExclusiveRename>(::dlsym(libc, "renameat2"));
        // libc is already a permanent dependency of this library.
        ::dlclose(libc);
        return result;
    }();
    return function;
}
#endif
#ifdef _WIN32
// Identity and containment use the original canonical filesystem paths. Only
// the final Win32 API spelling is extended, independent of process manifests
// or the machine's LongPathsEnabled policy.
std::wstring win32_file_path(const F::path& path)
{
    if (!path.is_absolute())
        throw Error("IO_ERROR", "Absolute private output required");
    const auto value = path.lexically_normal().native();
    if (value.rfind(L"\\\\?\\", 0) == 0)
        return value;
    if (value.rfind(L"\\\\", 0) == 0)
        return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
#endif
F::path io_path(const F::path& path)
{
#ifdef _WIN32
    return F::path(win32_file_path(path));
#else
    return path;
#endif
}
F::path identity_path(F::path path)
{
#ifdef _WIN32
    const auto value = path.native();
    if (value.rfind(L"\\\\?\\UNC\\", 0) == 0)
        return F::path(L"\\\\" + value.substr(8));
    if (value.rfind(L"\\\\?\\", 0) == 0)
        return F::path(value.substr(4));
#endif
    return path;
}
std::string read_private(const F::path& path) { return read(io_path(path)); }
F::path private_output(Context& c, const F::path& path)
{
#ifndef _WIN32
    return output(c, path.u8string());
#else
    const auto root     = identity_path(F::weakly_canonical(io_path(c.root)));
    const auto resolved = identity_path(F::weakly_canonical(io_path(path)));
    const auto relative = resolved.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        throw Error("OUTPUT_PATH", "Output must remain within workspace");
    for (const auto& source : c.protected_sources) {
        const auto original = F::u8path(source.first);
        if (resolved == original || (F::exists(io_path(resolved)) && F::equivalent(io_path(resolved), io_path(original))))
            throw Error("SOURCE_OVERWRITE", "Output aliases a protected source");
    }
    F::create_directories(io_path(resolved.parent_path()));
    return resolved;
#endif
}
bool publish_exclusive(const F::path& destination, const std::string& bytes, const F::path& source)
{
    static std::atomic<uint64_t> serial{0};
    auto existing = [&] {
        if (F::is_symlink(F::symlink_status(io_path(destination))) || !F::is_regular_file(io_path(destination)) ||
            F::equivalent(io_path(source), io_path(destination)) || read_private(destination) != bytes)
            throw Error("OUTPUT_CONFLICT", "Existing output is not the independent revalidated result");
        return false;
    };
    std::random_device random;
    const auto suffix       = sha256(std::to_string(random()) + ":" + std::to_string(random()) + ":" + std::to_string(++serial));
    const F::path temporary = destination.u8string() + ".tmp-" + suffix;
#ifdef _WIN32
    const auto temporary_api = win32_file_path(temporary), destination_api = win32_file_path(destination);
    HANDLE file = io_fault(IoBoundary::temp_create)?INVALID_HANDLE_VALUE:CreateFileW(temporary_api.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const auto error=GetLastError();
        throw Error("Cannot exclusively create private temporary output",IoBoundary::temp_create,io_windows(error));
    }
    try {
        for (size_t offset = 0; offset < bytes.size();) {
            DWORD written = 0, amount = DWORD(std::min<size_t>(bytes.size() - offset, 16 * 1024 * 1024));
            const auto ok=io_fault(IoBoundary::temp_write)?FALSE:WriteFile(file, bytes.data() + offset, amount, &written, nullptr);
#ifdef ORCA_MODEL_IO_TESTING
            const auto tested_ok=(!ok && io_test_code==-2)?TRUE:ok;
#else
            const auto tested_ok=ok;
#endif
            const auto error=tested_ok?ERROR_SUCCESS:GetLastError();
            if (!tested_ok || written != amount)
                throw Error("Incomplete private temporary write",IoBoundary::temp_write,tested_ok?IoKind::unknown:io_windows(error));
            offset += written;
        }
        if (io_fault(IoBoundary::temp_flush) || !FlushFileBuffers(file)) {
            const auto error=GetLastError();
            throw Error("Cannot flush private temporary output",IoBoundary::temp_flush,io_windows(error));
        }
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        if (io_fault(IoBoundary::output_publish) || !MoveFileExW(temporary_api.c_str(), destination_api.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const auto error=GetLastError();
            if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
                DeleteFileW(temporary_api.c_str());
                return existing();
            }
            throw Error("Cannot publish without replacing an existing file",IoBoundary::output_publish,io_windows(error));
        }
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        DeleteFileW(temporary_api.c_str());
        throw;
    }
#else
    int file = io_fault(IoBoundary::temp_create)? -1 : ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
    if (file < 0) {
        const int error=errno;
        throw Error("Cannot exclusively create private temporary output",IoBoundary::temp_create,io_errno(error));
    }
    try {
        for (size_t offset = 0; offset < bytes.size();) {
            const auto written = io_fault(IoBoundary::temp_write)? -1 : ::write(file, bytes.data() + offset, bytes.size() - offset);
            const int error=written<0?errno:0;
            if (written < 0 && error == EINTR)
                continue;
            if (written <= 0)
                throw Error("Incomplete private temporary write",IoBoundary::temp_write,written<0?io_errno(error):IoKind::unknown);
            offset += size_t(written);
        }
        if (io_fault(IoBoundary::temp_flush) || ::fsync(file) != 0) {
            const int error=errno;
            throw Error("Cannot flush private temporary output",IoBoundary::temp_flush,io_errno(error));
        }
        ::close(file);
        file = -1;
        bool consumed_temporary = false;
        const bool fault = io_fault(IoBoundary::output_publish);
        int status = -1;
        if (!fault) {
#ifdef __ANDROID__
            const auto published = android_exclusive_publish(android_get_device_api_level(), android_exclusive_rename(),
                                                             ::link, temporary.c_str(), destination.c_str());
            status = published.status;
            consumed_temporary = published.consumed_temporary;
#else
            status = ::link(temporary.c_str(), destination.c_str());
#endif
        }
        if (status != 0) {
            const int error=errno;
            if (error == EEXIST) {
                ::unlink(temporary.c_str());
                return existing();
            }
            throw Error("Cannot publish without replacing an existing file",IoBoundary::output_publish,io_errno(error));
        }
        if (!consumed_temporary) ::unlink(temporary.c_str());
    } catch (...) {
        if (file >= 0)
            ::close(file);
        ::unlink(temporary.c_str());
        throw;
    }
#endif
    return true;
}
constexpr size_t glb_vertices = 2500000, glb_faces = 2000000;
uint32_t u32(const std::string& bytes, size_t at)
{
    if (at > bytes.size() || bytes.size() - at < 4)
        throw Error("INVALID_GLB", "Truncated integer");
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + at);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void require(bool value, const char* message)
{
    if (!value)
        throw Error("INVALID_GLB", message);
}
F::path private_file(Context& c, const std::string& name)
{
    auto path = F::u8path(name);
    require(path.is_absolute(), "Source path must be absolute");
    auto root = identity_path(F::canonical(io_path(c.root))), normalized = path.lexically_normal();
    auto relative = normalized.lexically_relative(root);
    require(!relative.empty() && !relative.is_absolute() && *relative.begin() != "..", "Source is outside private workspace");
    auto current = root;
    for (const auto& part : relative) {
        current /= part;
        require(!F::is_symlink(F::symlink_status(io_path(current))), "Symbolic links are not allowed");
    }
    require(F::is_regular_file(io_path(current)), "Source must be a regular file");
    require(identity_path(F::canonical(io_path(current))) == normalized, "Source may not traverse a reparse point");
    return current;
}
J check(bool passed, size_t count = 0, const char* details = "")
{
    return {{"performed", true}, {"status", passed ? "passed" : "failed"}, {"count", count}, {"details", details}};
}
struct Glb
{
    J doc;
    const std::string& source;
    size_t bin = 0, bin_size = 0;
    Glb(std::string&&) = delete;
    explicit Glb(const std::string& bytes) : source(bytes)
    {
        require(source.size() >= 28 && u32(source, 0) == 0x46546c67 && u32(source, 4) == 2 && u32(source, 8) == source.size(),
                "Invalid GLB header");
        size_t n = u32(source, 12);
        require(n && n % 4 == 0 && n <= 8388608 && n <= source.size() - 28 && u32(source, 16) == 0x4e4f534a, "Invalid JSON chunk");
        doc      = J::parse(source.begin() + 20, source.begin() + 20 + n);
        bin      = n + 28;
        bin_size = u32(source, n + 20);
        require(u32(source, n + 24) == 0x004e4942 && bin_size % 4 == 0 && bin_size == source.size() - bin, "Invalid BIN chunk");
        require(doc.at("asset").at("version") == "2.0" && doc.at("buffers").size() == 1 && !doc.at("buffers")[0].contains("uri"),
                "Embedded GLB2 buffer required");
        const auto size = doc.at("buffers")[0].at("byteLength").get<size_t>();
        require(size <= bin_size && bin_size - size <= 3, "Incomplete BIN payload");
        for (const char* field : {"extensionsUsed", "extensionsRequired"}) {
            const auto extensions = doc.value(field, J::array());
            require(extensions.is_array() && extensions.size() <= 1, "Unsupported extension declaration");
            for (const auto& extension : extensions)
                require(extension.is_string() && extension == "KHR_materials_specular", "Unsupported extension declaration");
        }
        require(doc.value("extensionsRequired", J::array()).empty() || !doc.value("extensionsUsed", J::array()).empty(),
                "Required extension must be declared used");
        require(!doc.contains("extensions") && !doc.contains("skins") && !doc.contains("animations"),
                "Compressed, extended or animated GLB is not supported by this safety policy");
        for (const auto& image : doc.value("images", J::array())) {
            require(!image.contains("uri") && image.contains("bufferView"), "All texture images must be embedded");
            const auto image_view = view(image.at("bufferView").get<size_t>());
            const size_t at = image_view.first, length = image_view.second;
            const auto mime = image.at("mimeType").get<std::string>();
            require(length > 4 && length <= 20 * 1024 * 1024 && (mime == "image/png" || mime == "image/jpeg"),
                    "Unsupported encoded texture");
            const auto byte = [&](size_t offset) {
                require(offset < length, "Truncated image header");
                return unsigned(static_cast<unsigned char>(source[at + offset]));
            };
            size_t width = 0, height = 0;
            if (mime == "image/png") {
                require(length >= 33 && source.compare(at, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0 &&
                            source.compare(at + 12, 4, "IHDR") == 0,
                        "Invalid embedded PNG header");
                for (size_t k = 0; k < 4; ++k) {
                    width  = (width << 8) | byte(16 + k);
                    height = (height << 8) | byte(20 + k);
                }
            } else {
                require(byte(0) == 255 && byte(1) == 216, "Invalid embedded JPEG header");
                size_t offset = 2;
                while (offset + 4 <= length) {
                    require(byte(offset++) == 255, "Invalid JPEG marker");
                    while (offset < length && byte(offset) == 255)
                        ++offset;
                    const auto marker = byte(offset++);
                    if (marker == 0xd9 || marker == 0xda)
                        break;
                    if (marker == 1 || (marker >= 0xd0 && marker <= 0xd7))
                        continue;
                    const size_t n = (byte(offset) << 8) | byte(offset + 1);
                    require(n >= 2 && n <= length - offset, "Invalid JPEG segment");
                    if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) || (marker >= 0xc9 && marker <= 0xcb) ||
                        (marker >= 0xcd && marker <= 0xcf)) {
                        require(n >= 8, "Truncated JPEG frame");
                        height = (byte(offset + 3) << 8) | byte(offset + 4);
                        width  = (byte(offset + 5) << 8) | byte(offset + 6);
                        break;
                    }
                    offset += n;
                }
            }
            require(width > 0 && height > 0 && width <= 67108864 / height, "Texture dimensions exceed desktop safety limit");
        }
        for (const auto& texture : doc.value("textures", J::array())) {
            item("images", texture.at("source").get<size_t>());
            if (texture.contains("sampler"))
                item("samplers", texture.at("sampler").get<size_t>());
            require(!texture.contains("extensions"), "Extended textures unsupported");
        }
        for (const auto& material : doc.value("materials", J::array())) {
            const auto validate_texture = [&](const J& info) {
                item("textures", info.at("index").get<size_t>());
                require(info.value("texCoord", 0) == 0 && !info.contains("extensions"), "Only untransformed TEXCOORD_0 supported");
            };
            for (const char* name : {"normalTexture", "occlusionTexture", "emissiveTexture"})
                if (material.contains(name))
                    validate_texture(material.at(name));
            const auto pbr = material.value("pbrMetallicRoughness", J::object());
            for (const char* name : {"baseColorTexture", "metallicRoughnessTexture"})
                if (pbr.contains(name))
                    validate_texture(pbr.at(name));
            if (material.contains("extensions")) {
                const auto& extensions = material.at("extensions");
                require(extensions.is_object() && extensions.size() == 1 && extensions.contains("KHR_materials_specular") &&
                            !doc.value("extensionsUsed", J::array()).empty(), "Unsupported material extension");
                const auto& specular = extensions.at("KHR_materials_specular");
                require(specular.is_object() && specular.size() == 1 && specular.contains("specularFactor") &&
                            specular.at("specularFactor").is_number(), "Only factor-only specular is supported");
                const auto factor = specular.at("specularFactor").get<double>();
                require(std::isfinite(factor) && factor >= 0 && factor <= 1, "Invalid specular factor");
            }
        }
    }
    const J& item(const char* list, size_t index) const
    {
        const auto& array = doc.at(list);
        require(array.is_array() && index < array.size(), "Invalid GLB reference");
        return array[index];
    }
    std::pair<size_t, size_t> view(size_t index) const
    {
        const auto& v = item("bufferViews", index);
        require(v.at("buffer") == 0 && !v.contains("extensions"), "External/compressed view unsupported");
        size_t offset = v.value("byteOffset", size_t(0)), length = v.at("byteLength").get<size_t>();
        require(offset <= bin_size && length <= bin_size - offset, "View outside BIN");
        return {bin + offset, length};
    }
    template<class Value = double>
    std::vector<Value> accessor(size_t index, const std::string& type, bool indices = false) const
    {
        const auto& a = item("accessors", index);
        require(a.at("type") == type && !a.contains("sparse") && !a.value("normalized", false), "Unsupported accessor layout");
        const auto component = a.at("componentType").get<unsigned>();
        require(indices ? (component == 5121 || component == 5123 || component == 5125) : component == 5126,
                "Unsupported accessor component");
        size_t components = type == "VEC3" ? 3 : type == "VEC2" ? 2 : 1;
        size_t count = a.at("count").get<size_t>(), width = component == 5121 ? 1 : component == 5123 ? 2 : 4;
        require(count && count <= (indices ? glb_faces * 3 : glb_vertices), "Accessor count exceeds policy");
        const auto id        = a.at("bufferView").get<size_t>();
        auto [start, length] = view(id);
        size_t offset = a.value("byteOffset", size_t(0)), stride = item("bufferViews", id).value("byteStride", components * width);
        require(stride >= components * width && stride <= 252 && stride % width == 0 && offset % width == 0 && offset <= length &&
                    components * width <= length - offset && count - 1 <= (length - offset - components * width) / stride,
                "Accessor outside view");
        std::vector<Value> values;
        values.reserve(count * components);
        for (size_t i = 0; i < count; ++i)
            for (size_t j = 0; j < components; ++j) {
                size_t at = start + offset + i * stride + j * width;
                double value;
                if (component == 5126) {
                    uint32_t bits = u32(source, at);
                    float f;
                    std::memcpy(&f, &bits, 4);
                    value = f;
                } else if (width == 4)
                    value = u32(source, at);
                else if (width == 2)
                    value = static_cast<unsigned char>(source[at]) | unsigned(static_cast<unsigned char>(source[at + 1])) << 8;
                else
                    value = static_cast<unsigned char>(source[at]);
                require(std::isfinite(value) && (indices || std::abs(value) <= 1e8), "Invalid attribute value");
                values.push_back(value);
            }
        return values;
    }
};
#include "glb_scene_transform.inc"
struct Topology
{
    std::vector<V> vertices;
    std::vector<T> faces;
    size_t degenerate = 0, duplicate = 0, boundary = 0, nonmanifold = 0, inconsistent = 0;
    std::vector<size_t> primitive_faces;
    SceneTransform world;
};
// Contiguous exact counts, no per-face/per-edge tree nodes. Sorting changes
// only the counting workspace, never the output triangle or vertex order.
std::array<size_t,5> compact_topology_counts(Context& c,const std::vector<V>& vertices,const std::vector<T>& faces)
{
    std::vector<T> keys;keys.reserve(faces.size());
    std::vector<uint64_t> edges;edges.reserve(faces.size()*3);
    size_t degenerate=0,duplicate=0,boundary=0,nonmanifold=0,inconsistent=0;
    for(size_t i=0;i<faces.size();++i) {
        if((i&4095)==0)check_cancel(c);
        const auto& f=faces[i];const auto& a=vertices.at(f[0]);const auto& b=vertices.at(f[1]);const auto& d=vertices.at(f[2]);
        V u{b[0]-a[0],b[1]-a[1],b[2]-a[2]},v{d[0]-a[0],d[1]-a[1],d[2]-a[2]};
        if(V{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]}==V{})++degenerate;
        auto key=f;std::sort(key.begin(),key.end());keys.push_back(key);
        for(size_t k=0;k<3;++k){auto x=f[k],y=f[(k+1)%3];edges.push_back(((uint64_t(std::min(x,y))<<32)|std::max(x,y))*2+(x<y?0:1));}
    }
    check_cancel(c);std::sort(keys.begin(),keys.end());check_cancel(c);
    for(size_t i=1;i<keys.size();++i)if(keys[i]==keys[i-1])++duplicate;
    keys.clear();keys.shrink_to_fit();
    std::sort(edges.begin(),edges.end());check_cancel(c);
    for(size_t i=0;i<edges.size();) {
        const auto key=edges[i]>>1;size_t forward=0,reverse=0;
        do{if(edges[i]&1)++reverse;else ++forward;++i;}while(i<edges.size()&&(edges[i]>>1)==key);
        const auto total=forward+reverse;if(total==1)++boundary;if(total>2)++nonmanifold;
        if(total==2&&(forward==2||reverse==2))++inconsistent;
    }
    return {degenerate,duplicate,boundary,nonmanifold,inconsistent};
}
Topology topology(Context& c, const Glb& g)
{
    Topology out;
    std::map<V, uint32_t> canonical;
    out.world = scene_world_transform(g);
    size_t total_position_vertices = 0;
    for (const auto& primitive : g.doc.at("meshes")[0].at("primitives")) {
        check_cancel(c);
        require(primitive.value("mode", 4) == 4 && !primitive.contains("targets") && !primitive.contains("extensions"),
                "Static dense triangles required");
        const auto& attrs = primitive.at("attributes");
        if (primitive.contains("material")) {
            const auto& material = g.item("materials", primitive.at("material").get<size_t>());
            if (material.dump().find("Texture") != std::string::npos)
                require(attrs.contains("TEXCOORD_0"), "Textured primitive is missing texture coordinates");
        }
        auto p       = g.accessor(attrs.at("POSITION").get<size_t>(), "VEC3");
        size_t count = p.size() / 3;
        require(count <= glb_vertices - total_position_vertices, "Combined vertex count exceeds safety policy");
        total_position_vertices += count;
        std::vector<uint32_t> remap;
        remap.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if ((i & 4095) == 0)
                check_cancel(c);
            V value = out.world.position({p[i * 3], p[i * 3 + 1], p[i * 3 + 2]});
            auto [it, inserted] = canonical.emplace(value, uint32_t(out.vertices.size()));
            if (inserted)
                out.vertices.push_back(value);
            remap.push_back(it->second);
        }
        for (const auto& [name, ref] : attrs.items()) {
            require(name == "POSITION" || name == "NORMAL" || name == "TEXCOORD_0", "Attribute unsupported by safety policy");
            if (name == "POSITION")
                continue;
            auto values = g.accessor(ref.get<size_t>(), name == "NORMAL" ? "VEC3" : "VEC2");
            require(values.size() == count * (name == "NORMAL" ? 3 : 2), "Attribute count mismatch");
        }
        std::vector<uint32_t> indexes;
        if (primitive.contains("indices"))
            indexes = g.accessor<uint32_t>(primitive.at("indices").get<size_t>(), "SCALAR", true);
        else {
            indexes.resize(count);
            std::iota(indexes.begin(), indexes.end(), 0.);
        }
        require(indexes.size() % 3 == 0 && out.faces.size() + indexes.size() / 3 <= glb_faces, "Invalid triangle count");
        out.primitive_faces.push_back(indexes.size() / 3);
        for (size_t i = 0; i < indexes.size(); i += 3) {
            if ((i & 4095) == 0)
                check_cancel(c);
            T face;
            for (size_t j = 0; j < 3; ++j) {
                require(indexes[i + j] < count, "Index out of bounds");
                face[j] = remap[size_t(indexes[i + j])];
            }
            if (out.world.reflected()) std::swap(face[1], face[2]);
            out.faces.push_back(face);
        }
    }
    require(!out.faces.empty() && out.vertices.size() >= 3, "Empty model");
    canonical.clear();
    const auto counts=compact_topology_counts(c,out.vertices,out.faces);
    out.degenerate=counts[0];out.duplicate=counts[1];out.boundary=counts[2];out.nonmanifold=counts[3];out.inconsistent=counts[4];
    return out;
}
void put32(std::string& bytes, uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(char(value >> shift));
}
std::string filtered_glb(const Glb& original, const std::set<size_t>& removed)
{
    J doc           = original.doc;
    std::string bin = original.source.substr(original.bin, doc.at("buffers")[0].at("byteLength").get<size_t>());
    size_t face     = 0;
    for (auto& primitive : doc.at("meshes")[0].at("primitives")) {
        const auto position_count =
            original.item("accessors", primitive.at("attributes").at("POSITION").get<size_t>()).at("count").get<size_t>();
        std::vector<uint32_t> indexes;
        if (primitive.contains("indices"))
            indexes = original.accessor<uint32_t>(primitive.at("indices").get<size_t>(), "SCALAR", true);
        else {
            indexes.resize(position_count);
            std::iota(indexes.begin(), indexes.end(), 0.);
        }
        while (bin.size() % 4)
            bin.push_back(0);
        const auto offset = bin.size();
        size_t kept       = 0;
        for (size_t i = 0; i < indexes.size(); i += 3, ++face)
            if (!removed.count(face)) {
                for (size_t k = 0; k < 3; ++k)
                    put32(bin, uint32_t(indexes[i + k]));
                kept += 3;
            }
        require(kept > 0, "Repair would remove a complete material primitive");
        const auto view = doc.at("bufferViews").size();
        doc["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", offset}, {"byteLength", kept * 4}, {"target", 34963}});
        const auto accessor = doc.at("accessors").size();
        doc["accessors"].push_back({{"bufferView", view}, {"componentType", 5125}, {"type", "SCALAR"}, {"count", kept}});
        primitive["indices"] = accessor;
    }
    doc["buffers"][0]["byteLength"] = bin.size();
    while (bin.size() % 4)
        bin.push_back(0);
    auto json = doc.dump();
    while (json.size() % 4)
        json.push_back(' ');
    std::string result;
    put32(result, 0x46546c67);
    put32(result, 2);
    put32(result, uint32_t(28 + json.size() + bin.size()));
    put32(result, uint32_t(json.size()));
    put32(result, 0x4e4f534a);
    result += json;
    put32(result, uint32_t(bin.size()));
    put32(result, 0x004e4942);
    result += bin;
    return result;
}
std::set<size_t> safe_removals(Context& c, const Topology& mesh)
{
    std::map<T, std::vector<size_t>> groups;
    std::map<std::pair<uint32_t, uint32_t>, size_t> edges;
    for (size_t i = 0; i < mesh.faces.size(); ++i) {
        if ((i & 4095) == 0)
            check_cancel(c);
        auto key = mesh.faces[i];
        std::sort(key.begin(), key.end());
        groups[key].push_back(i);
        for (size_t j = 0; j < 3; ++j) {
            auto x = mesh.faces[i][j], y = mesh.faces[i][(j + 1) % 3];
            ++edges[{std::min(x, y), std::max(x, y)}];
        }
    }
    std::set<size_t> removed;
    for (const auto& [key, ids] : groups)
        if (ids.size() == 2) {
            const auto& f = mesh.faces[ids[0]];
            bool isolated = true;
            for (size_t j = 0; j < 3; ++j) {
                auto x = f[j], y = f[(j + 1) % 3];
                isolated &= edges.at({std::min(x, y), std::max(x, y)}) == 2;
            }
            // Only isolated coincident two-face shells. An attached duplicate,
            // seam, hole or non-manifold fan is never guessed into a repair.
            if (isolated) {
                removed.insert(ids[0]);
                removed.insert(ids[1]);
            }
        }
    return removed;
}
#ifdef ORCA_MODEL_HAS_REPAIR_CORE
#include "repair_core/EnhancedGlbCarrier.inc"
#endif
} // namespace

J glb_safety(Context& c, const J& request)
{
    const auto operation_started=std::chrono::steady_clock::now();
    struct Deadline
    {
        Context& context;
        explicit Deadline(Context& c) : context(c)
        {
            context.safety_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        }
        ~Deadline() { context.safety_deadline = {}; }
    } deadline(c);
    require(!request.contains("policyVersion") || request.at("policyVersion")=="glb-safety-v1" || request.at("policyVersion")=="glb-repair-v2", "Unknown GLB repair policy");
    const auto identity = request.at("identity");
    require(identity.is_object() && identity.size() == 6, "Safety identity must contain exactly the six bound fields");
    for (const char* field : {"accountId", "jobId", "assetId", "sourceSha256", "operationId"}) {
        const auto value = identity.at(field).get<std::string>();
        require(!value.empty() && value.size() <= 128, "Invalid safety identity");
    }
    require(identity.at("assetVersion").is_number_unsigned() && identity.at("assetVersion").get<uint64_t>() > 0, "Invalid asset version");
    uint64_t sequence = 0;
    auto event        = [&](const char* phase) {
        check_cancel(c);
        if (c.event) {
            auto text = J{{"schemaVersion", 1},
                                 {"type", "glbSafetyProgress"},
                                 {"identity", identity},
                                 {"sequence", ++sequence},
                                 {"phase", phase}}
                            .dump();
            c.event(text.c_str(), c.event_user);
        }
        check_cancel(c);
    };
    event("reading");
    const auto path  = private_file(c, request.at("sourcePath").get<std::string>());
    const auto bytes = read_private(path), digest = sha256(bytes);
    require(digest == identity.at("sourceSha256"), "Source SHA256 mismatch");
    event("structure");
    const Glb g(bytes);
    uint64_t estimated_vertices = 0, estimated_faces = 0;
    for (const auto& model : g.doc.at("meshes"))
        for (const auto& primitive : model.at("primitives")) {
            const auto count = g.item("accessors", primitive.at("attributes").at("POSITION").get<size_t>()).at("count").get<uint64_t>();
            require(count <= glb_vertices - estimated_vertices, "Combined vertex policy exceeded");
            estimated_vertices += count;
            const auto indexes = primitive.contains("indices") ?
                                     g.item("accessors", primitive.at("indices").get<size_t>()).at("count").get<uint64_t>() :
                                     count;
            require(indexes % 3 == 0 && indexes / 3 <= glb_faces - estimated_faces, "Combined triangle policy exceeded");
            estimated_faces += indexes / 3;
        }
    J workspace_plan=nullptr;
    uint64_t resource_estimate=uint64_t(bytes.size())*4+estimated_vertices*96+estimated_faces*448;
    if(request.value("policyVersion",std::string())=="glb-repair-v2") {
        // Phase maxima, not a replacement global face coefficient. Later carrier
        // allocations are separately gated on actual split count/AABB lifetime.
        const uint64_t Vn=estimated_vertices,Fn=estimated_faces,B=bytes.size();
        const uint64_t reserve=128ull*1024*1024+uint64_t(g.bin)*16;
        const uint64_t map_node=sizeof(std::pair<const V,uint32_t>)+4*sizeof(void*)+32;
        const uint64_t decode=B+std::max(Vn*(4*sizeof(V)+sizeof(uint32_t)+map_node),Vn*(3*sizeof(V)+sizeof(uint32_t)+map_node)+Fn*(2*sizeof(T)+3*sizeof(uint32_t)));
        const uint64_t counting=B+Vn*2*sizeof(V)+Fn*(3*sizeof(T)+3*sizeof(uint64_t));
        const uint64_t dense=Vn*sizeof(V)+Fn*sizeof(T),core_float=Vn*12+Fn*12;
        const uint64_t core=B+3*dense+2*core_float+Vn*32+Fn*64;
        // Surface connectivity/points and predicate boxes coexist with both
        // immutable input and repaired output. CGAL is not an OS hard RSS cap.
        const uint64_t directed_edge_node=sizeof(std::pair<const uint32_t,uint32_t>)+4*sizeof(void*)+32;
        const uint64_t soup_validation=B+2*dense+Fn*3*directed_edge_node;
        const uint64_t residual=std::max(soup_validation,B+3*dense+Vn*(sizeof(V)+32)+Fn*(3*16+8+72));
        resource_estimate=std::max({decode,counting,core,residual})+reserve;
        workspace_plan={{"policy","phase-live-containers-v2"},{"decodeBytes",decode},{"countingBytes",counting},
            {"coreBytes",core},{"soupValidationBytes",soup_validation},{"residualBytes",residual},{"reserveBytes",reserve},{"admissionBytes",resource_estimate},
            {"carrierCheckedDynamically",true},{"hardRssLimit",false}};
    }
    if (resource_estimate > 768ull * 1024 * 1024)
        throw Error("RESOURCE_LIMIT", "Estimated GLB safety workspace exceeds 768 MiB policy");
    event("geometry");
    auto mesh = topology(c, g);
    if (request.value("policyVersion", std::string()) == "glb-repair-v2") {
#ifdef ORCA_MODEL_HAS_REPAIR_CORE
        require(request.at("operation") == "repairGlbArtifact", "Repair v2 requires repair operation");
        event("repairing");
        const auto core_started=std::chrono::steady_clock::now();
        require(g.doc.at("meshes")[0].at("primitives").size()==1,"Enhanced profile requires one material primitive");
        auto proof = enhanced_glb_geometry(c, mesh.vertices, mesh.faces,[&](int progress){
            check_cancel(c);
            if(c.event){auto message=J{{"schemaVersion",1},{"type","glbSafetyProgress"},{"identity",identity},{"sequence",++sequence},{"phase","repairing"},{"progress",progress}}.dump();c.event(message.c_str(),c.event_user);}
            check_cancel(c);
        });
#ifdef ORCA_MODEL_HAS_GLB_CGAL
        const auto residual_started=std::chrono::steady_clock::now();
        event("verifying");
        proof["topologyCounts"]=enhanced_topology_counts(c,mesh.vertices,mesh.faces);
        proof["residualChecks"]=glb_strict_predicates(c,mesh.vertices,mesh.faces,event);
#endif
        proof["workspaceAdmission"]=workspace_plan;
        event("appearance");
        const auto carrier_started=std::chrono::steady_clock::now();
        // Original geometry is needed only by projection, not simultaneously
        // with Core repair or CGAL scratch. Rebuild from the immutable source.
        auto input=topology(c,g);
        auto result=enhanced_glb_carrier(c,g,input,mesh,request,proof,path,event);
        J phase{{"decodePreflightMs",std::chrono::duration_cast<std::chrono::milliseconds>(core_started-operation_started).count()},
                {"coreMs",proof.at("wallMs")},
                {"carrierTotalMs",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-carrier_started).count()},
                {"operationTotalMs",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-operation_started).count()}};
#ifdef ORCA_MODEL_HAS_GLB_CGAL
        phase["residualMs"]=std::chrono::duration_cast<std::chrono::milliseconds>(carrier_started-residual_started).count();
#endif
        result["phaseWallMilliseconds"]=phase;
        return result;
#else
        throw Error("UNSUPPORTED", "Desktop enhanced repair Core is not compiled");
#endif
    }

    std::string candidate_bytes;
    bool appearance_equivalent = true;
    J geometry_deviation = {{"performed", false}, {"status", "not_run"}, {"count", nullptr}, {"details", "No topology repair performed"}};
#ifdef ORCA_MODEL_HAS_GLB_CGAL
    if (request.at("operation") == "repairGlbArtifact" && mesh.duplicate) {
        event("repair");
        const auto removed = safe_removals(c, mesh);
        if (!removed.empty() && removed.size() <= 64) {
            // Until per-corner texture/normal interpolation equivalence is
            // implemented, automatic deletion is confined to one untextured,
            // normal-free material primitive and exact redundant coverage.
            const auto& primitives = g.doc.at("meshes")[0].at("primitives");
            appearance_equivalent  = primitives.size() == 1 && !primitives[0].contains("material") &&
                                    g.doc.value("materials", J::array()).empty() && !primitives[0].at("attributes").contains("NORMAL") &&
                                    !primitives[0].at("attributes").contains("TEXCOORD_0") && g.doc.value("textures", J::array()).empty() &&
                                    g.doc.value("images", J::array()).empty();
            geometry_deviation = glb_removed_deviation(c, mesh.vertices, mesh.faces, removed);
            appearance_equivalent &= geometry_deviation.at("details").at("redundantSurfaceCover").get<bool>();
            if (geometry_deviation.at("status") == "passed" && appearance_equivalent) {
                candidate_bytes = filtered_glb(g, removed);
                mesh            = topology(c, Glb(candidate_bytes));
            }
        }
    }
#endif
    event("topology");
    const auto defects = mesh.boundary + mesh.nonmanifold + mesh.degenerate + mesh.duplicate + mesh.inconsistent;
    J checks{{"structure", check(true)},
             {"geometry", check(true)},
             {"appearance", check(true, 0, "Original GLB byte identity preserved; no texture or material conversion")},
             {"topology", check(defects == 0, defects)},
             {"selfIntersection",
              {{"performed", false},
               {"status", "unsupported"},
               {"count", nullptr},
               {"details", "CGAL strict self-intersection backend is not compiled"}}}};
    checks["geometryDeviation"] = geometry_deviation;
    if (!appearance_equivalent)
        checks["appearance"] = check(false, 1, "Removed surface/corner attributes are not proven appearance-equivalent");
    checks["topology"]["details"] = {{"boundaryEdges", mesh.boundary},         {"nonManifoldEdges", mesh.nonmanifold},
                                     {"degenerateFaces", mesh.degenerate},     {"duplicateFaces", mesh.duplicate},
                                     {"inconsistentEdges", mesh.inconsistent}, {"nonManifoldVertices", nullptr}};
#ifdef ORCA_MODEL_HAS_GLB_CGAL
    checks["selfIntersection"] = {{"performed", false},
                                  {"status", "not_run"},
                                  {"count", nullptr},
                                  {"details", "Topology defects must be resolved before strict predicates"}};
    if (defects == 0) {
        const auto strict = glb_strict_predicates(c, mesh.vertices, mesh.faces);
        if (!strict.at("topologyValid").get<bool>()) {
            checks["topology"]            = check(false, 1, "Non-manifold vertex link or incompatible surface topology");
            checks["topology"]["details"] = strict;
            checks["selfIntersection"]    = {{"performed", false},
                                             {"status", "not_run"},
                                             {"count", nullptr},
                                             {"details", "Topology must pass before strict self-intersection checking"}};
        } else {
            checks["topology"]["details"]["nonManifoldVertices"] = 0;
            checks["selfIntersection"] = check(!strict.at("selfIntersects").get<bool>(), strict.at("selfIntersects").get<bool>() ? 1 : 0,
                                               "CGAL EPICK does_self_intersect predicate; count is a risk flag");
        }
    }
#endif
    // No writer may publish a candidate on incomplete strict checks. This is
    // an explicit capability limit, never a fabricated successful repair.
    event("revalidation");
    require(sha256(read_private(path)) == digest, "Source changed during checking");
    bool verified = true, failed = false;
    for (const char* name : {"structure", "geometry", "topology", "selfIntersection", "appearance"}) {
        verified &= checks.at(name).at("performed").get<bool>() && checks.at(name).at("status") == "passed";
        failed |= checks.at(name).at("status") == "failed";
    }
    for (const auto& entry : checks.items()) {
        if (entry.value().at("status") == "failed") {
            failed   = true;
            verified = false;
        }
    }
    const bool repair_applied = !candidate_bytes.empty();
    if (verified && request.at("operation") == "repairGlbArtifact" && candidate_bytes.empty())
        candidate_bytes = bytes;
    if (candidate_bytes.size() > max_bytes)
        throw Error("RESOURCE_LIMIT", "Derived GLB exceeds the 100 MiB output policy");
    J candidate = nullptr;
    F::path receipt_path;
    if (verified && !candidate_bytes.empty()) {
        event("publishing");
        const auto directory = request.at("outputDirectory").get<std::string>();
        const auto dir       = F::u8path(directory);
        require(dir.is_absolute() && F::is_directory(io_path(dir)), "Existing private output directory required");
        const auto operation_hash = sha256(identity.dump());
        const auto destination    = private_output(c, dir / (operation_hash + (repair_applied ? "-repaired.glb" : "-verified.glb")));
        receipt_path              = private_output(c, dir / (operation_hash + ".glb-safety.json"));
        for (const auto& target : {destination, receipt_path}) {
            require(identity_path(F::weakly_canonical(io_path(target))) != identity_path(F::canonical(io_path(path))),
                    "Output must be an independent file");
            require(!F::exists(io_path(target)) || !F::equivalent(io_path(path), io_path(target)), "Output aliases the immutable original");
        }
        // Prove the entire original BIN prefix, material JSON and every
        // non-index accessor are preserved before publishing the new version.
        const Glb derived(candidate_bytes);
        require(derived.source.compare(derived.bin, g.doc.at("buffers")[0].at("byteLength").get<size_t>(), bytes, g.bin,
                                       g.doc.at("buffers")[0].at("byteLength").get<size_t>()) == 0,
                "Original BIN payload changed");
        require(derived.doc.value("materials", J::array()) == g.doc.value("materials", J::array()) &&
                    derived.doc.value("images", J::array()) == g.doc.value("images", J::array()) &&
                    derived.doc.value("textures", J::array()) == g.doc.value("textures", J::array()),
                "Appearance definitions changed");
        checks["appearance"]["details"] = {{"originalBinSha256",
                                            sha256(bytes.substr(g.bin, g.doc.at("buffers")[0].at("byteLength").get<size_t>()))},
                                           {"encodedImagePayloadsPreserved", true},
                                           {"materialDefinitionsPreserved", true},
                                           {"uvAccessorBytesPreserved", true},
                                           {"strategy",
                                            repair_applied ? "isolated_coincident_two_face_shell_removal" : "verified_unchanged_copy"}};
        const bool existed              = F::exists(io_path(destination));
        bool created                    = false;
        if (existed) {
            require(!F::is_symlink(F::symlink_status(io_path(destination))) && F::is_regular_file(io_path(destination)),
                    "Invalid existing candidate");
            require(read_private(destination) == candidate_bytes, "Existing candidate differs from revalidated source-derived result");
        } else
            created = publish_exclusive(destination, candidate_bytes, path);
        try {
            check_cancel(c);
            require(sha256(read_private(destination)) == sha256(candidate_bytes), "Published candidate SHA mismatch");
            require(sha256(read_private(path)) == digest, "Original changed before publication");
        } catch (...) {
            std::error_code ec;
            if (created) {
#ifdef _WIN32
                DeleteFileW(win32_file_path(destination).c_str());
#else
                F::remove(destination, ec);
#endif
            }
            throw;
        }
        candidate = {{"path", destination.u8string()},
                     {"sha256", sha256(candidate_bytes)},
                     {"bytes", candidate_bytes.size()},
                     {"format", "glb"},
                     {"mediaType", "model/gltf-binary"}};
    }
    J result =
        {{"policyVersion", "glb-safety-v1"},
         {"identity", identity},
         {"source",
          {{"path", path.u8string()}, {"sha256", digest}, {"bytes", bytes.size()}, {"format", "glb"}, {"mediaType", "model/gltf-binary"}}},
         {"candidate", candidate},
         {"checks", checks},
         {"repairApplied", !candidate.is_null() && repair_applied},
         {"completion", verified ? "verified" :
                        failed   ? "needs_attention" :
                                   "unsupported"},
         {"vertices", mesh.vertices.size()},
         {"triangles", mesh.faces.size()},
         {"geometryVerified", verified},
         {"appearanceVerified", appearance_equivalent}};
    result["resourceEstimateBytes"] = resource_estimate;
    if (!candidate.is_null()) {
        check_cancel(c);
        const auto receipt = result.dump();
        require(receipt.size() <= 65536, "Safety receipt size limit");
        if (F::exists(io_path(receipt_path))) {
            require(!F::is_symlink(F::symlink_status(io_path(receipt_path))) && F::is_regular_file(io_path(receipt_path)) &&
                        F::file_size(io_path(receipt_path)) <= 65536,
                    "Invalid existing receipt");
            require(read_private(receipt_path) == receipt, "Stored receipt does not match revalidated identity and proof");
        } else
            publish_exclusive(receipt_path, receipt, path);
        result["receipt"] = {{"path", receipt_path.u8string()}, {"sha256", sha256(receipt)}, {"bytes", receipt.size()}};
    }
    return result;
}
} // namespace orca_model_detail

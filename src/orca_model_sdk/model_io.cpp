#include "model_internal.hpp"
namespace orca_model_detail {
void valid(const Mesh& m)
{
    if (m.vertices.size() < 3 || m.faces.empty())
        throw Error("INVALID_GEOMETRY", "Mesh requires vertices and triangles");
    if (m.vertices.size() > max_vertices || m.faces.size() > max_faces)
        throw Error("RESOURCE_LIMIT", "Mesh exceeds SDK limits");
    for (const auto& v : m.vertices)
        for (double n : v)
            if (!std::isfinite(n) || std::abs(n) > 1e8)
                throw Error("INVALID_GEOMETRY", "Invalid coordinate");
    for (const auto& t : m.faces)
        for (auto i : t)
            if (i >= m.vertices.size())
                throw Error("INVALID_INDEX", "Triangle references missing vertex");
    if (!m.colors.empty() && m.colors.size() != m.faces.size())
        throw Error("INVALID_APPEARANCE", "Face color count does not match geometry");
    if (!m.materials.empty() && m.materials.size() != m.faces.size())
        throw Error("INVALID_APPEARANCE", "Face material count does not match geometry");
    for (const auto& color : m.colors)
        for (double value : color)
            if (!std::isfinite(value) || value < 0 || value > 1)
                throw Error("INVALID_COLOR", "Face colors require finite normalized RGBA");
    for (auto material : m.materials)
        if (material < 0 || material > 255)
            throw Error("INVALID_MATERIAL", "Material ID must be 0..255");
}
std::string read(const F::path& p)
{
    std::error_code ec;
    auto n = F::file_size(p, ec);
    if (ec || n == 0 || n > max_bytes)
        throw Error("FILE_LIMIT", "File missing, empty or above 100 MiB");
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw Error("IO_ERROR", "Cannot open input");
    std::string s(static_cast<size_t>(n), '\0');
    if (!f.read(s.data(), s.size()))
        throw Error("IO_ERROR", "Input changed while reading");
    return s;
}
F::path output(Context& c, const std::string& name, bool document_save)
{
    auto p = F::u8path(name);
    if (p.is_relative())
        p = c.root / p;
    auto resolved = F::weakly_canonical(p);
    auto root     = F::weakly_canonical(c.root);
    auto relative = resolved.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        throw Error("OUTPUT_PATH", "Output must remain within workspace");
    const auto current_document = F::weakly_canonical(c.root / F::u8path("documents/" + c.doc.id + ".json"));
    for (const auto& source : c.protected_sources) {
        const auto original = F::u8path(source.first);
        std::error_code equivalent_error;
        const bool alias = resolved == original || (F::exists(resolved) && F::equivalent(resolved, original, equivalent_error));
        if (alias && !(document_save && resolved == current_document && original == current_document))
            throw Error("SOURCE_OVERWRITE", "Output would overwrite a protected source or document asset");
    }
    F::create_directories(resolved.parent_path());
    return resolved;
}
void protect_source(Document& document, const F::path& source)
{
    const auto canonical                   = F::canonical(source);
    document.sources[canonical.u8string()] = sha256(read(canonical));
}
void atomic_write(const F::path& p, const std::string& bytes)
{
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(bytes.data(), bytes.size()))
            throw Error("IO_ERROR", "Cannot write complete output");
    }
    std::error_code ec;
    F::rename(tmp, p, ec);
    if (ec) {
#ifdef _WIN32
        // Replace atomically. Deleting the old file before a second rename
        // could destroy the last durable document on a failed replacement.
        if (MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            ec.clear();
#endif
    }
    if (ec)
        throw Error("IO_ERROR", "Cannot commit output");
}
void defaults(Mesh& m)
{
    m.colors.resize(m.faces.size(), {0.8, 0.8, 0.8, 1});
    m.materials.resize(m.faces.size(), 0);
}
Mesh parse_mesh(const std::string& bytes, std::string extension, Context& c, bool converted)
{
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char n) { return char(std::tolower(n)); });
    Mesh m;
    if (extension == ".stl") {
        uint32_t count = 0;
        if (bytes.size() >= 84)
            std::memcpy(&count, bytes.data() + 80, 4);
        if (bytes.size() >= 84 && count > 0 && uint64_t(count) * 50 + 84 == bytes.size()) {
            if (count > max_faces)
                throw Error("RESOURCE_LIMIT", "Too many STL faces");
            for (uint32_t i = 0; i < count; ++i) {
                check_cancel(c);
                uint16_t attributes = 0;
                std::memcpy(&attributes, bytes.data() + 84 + i * 50 + 48, 2);
                if (attributes)
                    throw Error("CONVERSION_REQUIRED", "STL facet attributes/color require explicit platform interpretation");
                T t{};
                for (int j = 0; j < 3; ++j) {
                    V v{};
                    for (int a = 0; a < 3; ++a) {
                        float n;
                        std::memcpy(&n, bytes.data() + 84 + i * 50 + 12 + j * 12 + a * 4, 4);
                        v[a] = n;
                    }
                    t[j] = uint32_t(m.vertices.size());
                    m.vertices.push_back(v);
                }
                m.faces.push_back(t);
            }
        } else {
            std::istringstream in(bytes);
            std::string word;
            while (in >> word) {
                if (word == "vertex") {
                    V v;
                    if (!(in >> v[0] >> v[1] >> v[2]))
                        throw Error("INVALID_STL", "Bad vertex");
                    m.vertices.push_back(v);
                    if (m.vertices.size() % 3 == 0) {
                        auto n = uint32_t(m.vertices.size());
                        m.faces.push_back({n - 3, n - 2, n - 1});
                    }
                    if (m.faces.size() > max_faces)
                        throw Error("RESOURCE_LIMIT", "Too many faces");
                }
            }
            if (m.vertices.size() % 3)
                throw Error("INVALID_STL", "Incomplete triangle");
        }
    } else if (extension == ".obj") {
        std::istringstream input(bytes);
        std::string line;
        while (std::getline(input, line)) {
            check_cancel(c);
            std::istringstream in(line);
            std::string kind;
            in >> kind;
            if (kind == "v") {
                V v;
                if (!(in >> v[0] >> v[1] >> v[2]))
                    throw Error("INVALID_OBJ", "Bad vertex");
                std::string attributes;
                if (in >> attributes && attributes[0] != '#')
                    throw Error("CONVERSION_REQUIRED", "OBJ vertex color/weight needs appearance-preserving platform conversion");
                m.vertices.push_back(v);
            } else if (kind == "f") {
                T t{};
                std::string token;
                for (int j = 0; j < 3; ++j) {
                    if (!(in >> token))
                        throw Error("INVALID_OBJ", "Incomplete face");
                    auto part  = token.substr(0, token.find('/'));
                    size_t end = 0;
                    long long n;
                    try {
                        n = std::stoll(part, &end);
                    } catch (const std::exception&) {
                        throw Error("INVALID_OBJ", "Invalid vertex index");
                    }
                    if (end != part.size() || !n)
                        throw Error("INVALID_OBJ", "Invalid vertex index");
                    long long index = n < 0 ? static_cast<long long>(m.vertices.size()) + n : n - 1;
                    if (index < 0 || index >= static_cast<long long>(m.vertices.size()))
                        throw Error("INVALID_OBJ", "Face index out of range");
                    t[j] = uint32_t(index);
                }
                if (in >> token && token[0] != '#')
                    throw Error("CONVERSION_REQUIRED", "OBJ polygons require platform triangulation preserving appearance");
                m.faces.push_back(t);
            } else if ((kind == "o" || kind == "g") && !converted)
                throw Error("CONVERSION_REQUIRED", "OBJ entities require a verified part-range conversion manifest");
            else if (kind == "mtllib" || kind == "usemtl" || kind == "vt")
                throw Error("CONVERSION_REQUIRED", "OBJ materials/UV require platform conversion with dependency manifest");
            if (m.vertices.size() > max_vertices || m.faces.size() > max_faces)
                throw Error("RESOURCE_LIMIT", "Mesh limit");
        }
    } else if (extension == ".off") {
        std::istringstream in(bytes);
        std::string magic;
        size_t nv = 0, nf = 0, edges = 0;
        if (!(in >> magic >> nv >> nf >> edges))
            throw Error("INVALID_OFF", "Invalid OFF header");
        if (magic != "OFF")
            throw Error("CONVERSION_REQUIRED", "Extended OFF attributes require platform conversion");
        if (nv > max_vertices || nf > max_faces)
            throw Error("INVALID_OFF", "Unsupported OFF header");
        for (size_t i = 0; i < nv; ++i) {
            V v;
            if (!(in >> v[0] >> v[1] >> v[2]))
                throw Error("INVALID_OFF", "Bad vertex");
            m.vertices.push_back(v);
        }
        for (size_t i = 0; i < nf; ++i) {
            int n;
            T t;
            if (!(in >> n))
                throw Error("INVALID_OFF", "Invalid OFF face");
            if (n != 3)
                throw Error("CONVERSION_REQUIRED", "OFF polygons require platform triangulation");
            if (!(in >> t[0] >> t[1] >> t[2]))
                throw Error("INVALID_OFF", "Expected triangulated OFF");
            m.faces.push_back(t);
        }
    } else if (extension == ".ply") {
        std::istringstream in(bytes);
        std::string line;
        size_t nv = 0, nf = 0;
        bool ascii = false, ended = false;
        std::string element;
        std::vector<std::string> vertex_properties;
        bool supported_face_property = false;
        std::getline(in, line);
        if (line != "ply" && line != "ply\r")
            throw Error("INVALID_PLY", "Bad magic");
        while (std::getline(in, line)) {
            if (line.find("format ascii 1.0") == 0)
                ascii = true;
            std::istringstream row(line);
            std::string a, b;
            row >> a >> b;
            if (a == "element") {
                element = b;
                if (b == "vertex")
                    row >> nv;
                else if (b == "face")
                    row >> nf;
                else
                    throw Error("CONVERSION_REQUIRED", "PLY extended elements require platform conversion");
            } else if (a == "property") {
                if (element == "vertex") {
                    std::string property;
                    row >> property;
                    vertex_properties.push_back(property);
                } else if (element == "face") {
                    std::string count_type, index_type, name;
                    row >> count_type >> index_type >> name;
                    if (b != "list" || (name != "vertex_indices" && name != "vertex_index") || supported_face_property)
                        throw Error("CONVERSION_REQUIRED", "PLY face attributes require platform conversion");
                    supported_face_property = true;
                }
            }
            if (line == "end_header" || line == "end_header\r") {
                ended = true;
                break;
            }
        }
        if (!ascii || vertex_properties != std::vector<std::string>{"x", "y", "z"})
            throw Error("CONVERSION_REQUIRED", "Binary or attributed/reordered PLY requires platform conversion");
        if (!ended || nv > max_vertices || nf > max_faces || !supported_face_property)
            throw Error("INVALID_PLY", "Expected bounded triangulated PLY");
        for (size_t i = 0; i < nv; ++i) {
            std::getline(in, line);
            std::istringstream row(line);
            V v;
            if (!(row >> v[0] >> v[1] >> v[2]))
                throw Error("INVALID_PLY", "Bad vertex");
            m.vertices.push_back(v);
        }
        for (size_t i = 0; i < nf; ++i) {
            std::getline(in, line);
            std::istringstream row(line);
            int n;
            T t;
            if (!(row >> n) || n != 3 || !(row >> t[0] >> t[1] >> t[2]))
                throw Error("INVALID_PLY", "Expected triangle face");
            m.faces.push_back(t);
        }
    } else
        throw Error("CONVERSION_REQUIRED", "Format requires platform IO adapter or existing Slicing SDK");
    valid(m);
    defaults(m);
    return m;
}
} // namespace orca_model_detail

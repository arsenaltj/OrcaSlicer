#pragma once
#include "orca/model/model_sdk_c.h"
#include "orca/mobile/slicing_sdk_c.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace orca_model_detail {

using J                    = nlohmann::json;
namespace F                = std::filesystem;
using V                    = std::array<double, 3>;
using T                    = std::array<uint32_t, 3>;
constexpr size_t max_bytes = 100u * 1024u * 1024u, max_faces = 2500000, max_vertices = 7500000;
struct Error : std::runtime_error
{
    std::string code;
    Error(const char* c, const std::string& m) : runtime_error(m), code(c) {}
};
struct Mesh
{
    std::vector<V> vertices;
    std::vector<T> faces;
    std::vector<std::array<double, 4>> colors;
    std::vector<int> materials;
};
struct Document
{
    Mesh mesh;
    std::string id, input_sha;
    uint64_t revision = 1, geometry = 1;
    J appearance = J::object();
    std::map<std::string, std::string> sources;
};
struct Backend
{
    void* library                                          = nullptr;
    orca_mobile_slicing_context* context                   = nullptr;
    using Execute                                          = decltype(&orca_mobile_slicing_execute);
    Execute execute                                        = nullptr;
    decltype(&orca_mobile_slicing_cancel) cancel           = nullptr;
    decltype(&orca_mobile_slicing_destroy) destroy         = nullptr;
    decltype(&orca_mobile_slicing_free_string) free_string = nullptr;
    ~Backend()
    {
        if (context && destroy)
            destroy(context);
        // The native slicing runtime owns process-wide worker pools. Unloading
        // its module from a context destructor can wait for workers under the
        // Windows loader lock. Contexts are released; verified runtime modules
        // are intentionally retained until process exit.
    }
};
struct Context
{
    F::path root;
    std::mutex mutex;
    std::atomic<bool> cancel{false};
    Document doc, candidate;
    uint64_t candidate_base = 0;
    uint64_t generation = 0, candidate_generation = 0, candidate_geometry = 0;
    std::string candidate_document;
    std::map<std::string, std::string> protected_sources;
    std::string candidate_id;
    std::vector<Document> undo, redo;
    bool return_inline                       = false;
    orca_model_event_fn event                = nullptr;
    void* event_user                         = nullptr;
    orca_model_platform_request_fn transport = nullptr;
    void* transport_user                     = nullptr;
    std::shared_ptr<Backend> backend;
};

extern std::mutex registry_mutex;
extern std::unordered_map<uintptr_t, std::shared_ptr<Context>> registry;
extern std::atomic<uintptr_t> sequence;
std::shared_ptr<Context> acquire(orca_model_context*);
char* allocated(const std::string&);
void check_cancel(Context&);
void valid(const Mesh&);
std::string read(const F::path&);
F::path output(Context&, const std::string&, bool document_save = false);
void protect_source(Document&, const F::path&);
J adopt_document(Context&, Document);
J preview_document(Context&, const Document&);
void clear_candidate(Context&);
void atomic_write(const F::path&, const std::string&);
std::string sha256(const std::string&);
void defaults(Mesh&);
Mesh parse_mesh(const std::string&, std::string, Context&, bool converted = false);
J scene(const Document&, bool include_mesh = true, bool include_appearance = true);
J scene_descriptor(Context&, const Document&);
std::string appearance_sha256(const Mesh&);
#ifdef ORCA_MODEL_HAS_REGION_CORE
J orca_model_select_region(const Document&, const J&, std::atomic<bool>&);
#endif
void expected(Context&, const J&);
J commit(Context&, Document);
J execute(Context&, const J&);
J open_document(Context&, const J&);
J manufacturing_bridge(Context&, const J&);
} // namespace orca_model_detail

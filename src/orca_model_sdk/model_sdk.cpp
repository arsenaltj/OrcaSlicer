#include "model_internal.hpp"
using namespace orca_model_detail;
namespace {
std::mutex module_mutex;
std::map<std::string, std::pair<std::string, void*>> modules;
void* retained_module(const F::path& file)
{
    const auto canonical = F::canonical(file);
    auto name            = canonical.u8string();
#ifdef _WIN32
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) { return char(std::tolower(value)); });
#endif
    const auto fingerprint = sha256(read(canonical));
    std::lock_guard<std::mutex> lock(module_mutex);
    const auto found = modules.find(name);
    if (found != modules.end()) {
        if (found->second.first != fingerprint)
            throw Error("BACKEND_RESTART_REQUIRED", "Loaded native runtime changed; restart process");
        return found->second.second;
    }
    if (modules.size() >= 4)
        throw Error("BACKEND_RESTART_REQUIRED", "Runtime cache full; restart before switching SDK bundles");
#ifdef _WIN32
    void* library        = LoadLibraryExW(canonical.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto available = [&](const char* symbol) { return GetProcAddress(static_cast<HMODULE>(library), symbol) != nullptr; };
#else
    void* library        = dlopen(canonical.c_str(), RTLD_NOW | RTLD_LOCAL);
    const auto available = [&](const char* symbol) { return dlsym(library, symbol) != nullptr; };
#endif
    if (!library)
        return nullptr;
    for (const char* symbol : {"orca_mobile_slicing_create", "orca_mobile_slicing_execute", "orca_mobile_slicing_cancel",
                               "orca_mobile_slicing_destroy", "orca_mobile_slicing_free_string"}) {
        if (!available(symbol)) {
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(library));
#else
            dlclose(library);
#endif
            throw Error("INVALID_BACKEND_ABI", "Native runtime missing required symbols");
        }
    }
    modules.emplace(name, std::make_pair(fingerprint, library));
    return library;
}
} // namespace
extern "C" {
const char* orca_model_version() { return "orca-model-sdk/1.0.0"; }
const char* orca_model_capabilities()
{
    static const std::string value = [] {
        J capabilities = {{"schemaVersion", 1},
                          {"abiVersion", "1.0"},
                          {"sdkVersion", orca_model_version()},
                          {"sourceCommit", ORCA_MODEL_SOURCE_COMMIT},
                          {"sourceIdentityRequiresManifest", true},
                          {"nativeFormats", {"stl", "obj-triangulated-untextured", "off-triangulated", "ply-ascii-triangulated"}},
                          {"platformConversionFormats", {"stl", "obj", "ply", "off", "3mf", "3ds", "wrl", "dae", "step", "stp", "glb"}},
                          {"operations",
                           {"capabilities",
                            "inspect",
                            "import",
                            "importConverted",
                            "create",
                            "getScene",
                            "transform",
                            "paint",
                            "assignMaterial",
                            "undo",
                            "redo",
                            "save",
                            "load",
                            "pick",
                            "analyze",
                            "weldExact",
                            "prepareCandidate",
                            "acceptCandidate",
                            "discardCandidate",
                            "exportManufacturing",
                            "manufacturingSnapshot",
                            "sliceDocument",
                            "recommendDocument",
                            "platformAI",
                            "slicing"}},
                          {"nativeSlicing", false},
                          {"slicingBridge", true},
                          {"platformAIRequiresHostTransport", true},
                          {"multiMaterialExecution", false},
                          {"advancedRepair", false},
                          {"sceneFormat", "ORCASC01"}};
#ifdef ORCA_MODEL_HAS_REGION_CORE
        capabilities["operations"].push_back("selectRegion");
        capabilities["nativeRegionCore"] = true;
#else
        capabilities["nativeRegionCore"] = false;
#endif
        return capabilities.dump();
    }();
    return value.c_str();
}
orca_model_context* orca_model_create(const char* workspace)
{
    if (!workspace || !*workspace)
        return nullptr;
    try {
        auto c  = std::make_shared<Context>();
        c->root = F::absolute(F::u8path(workspace));
        F::create_directories(c->root);
        auto id = sequence.fetch_add(1);
        std::lock_guard<std::mutex> lock(registry_mutex);
        registry.emplace(id, std::move(c));
        return reinterpret_cast<orca_model_context*>(id);
    } catch (...) {
        return nullptr;
    }
}
int orca_model_execute(orca_model_context* handle, const char* request, orca_model_event_fn event, void* user, char** result)
{
    if (!result)
        return ORCA_MODEL_INVALID;
    *result  = nullptr;
    auto c   = acquire(handle);
    int code = ORCA_MODEL_OK;
    J response;
    try {
        if (!c || !request)
            throw Error("INVALID_ARGUMENT", "Invalid handle/request");
        if (std::strlen(request) > max_bytes)
            throw Error("RESOURCE_LIMIT", "Request too large");
        std::unique_lock<std::mutex> lock(c->mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw Error("BUSY", "Context operation running");
        c->cancel        = false;
        c->event         = event;
        c->event_user    = user;
        auto r           = J::parse(request);
        c->return_inline = r.value("returnInlineScene", false);
        if (event)
            event("{\"phase\":\"running\"}", user);
        auto payload = execute(*c, r);
        response     = {{"ok", true}, {"schemaVersion", 1}, {"result", std::move(payload)}};
    } catch (const Error& e) {
        code     = e.code == "CANCELED" ? ORCA_MODEL_CANCELED :
                   e.code == "BUSY"     ? ORCA_MODEL_BUSY :
                   (e.code == "UNSUPPORTED_OPERATION" || e.code == "CONVERSION_REQUIRED" || e.code == "REGION_CORE_UNAVAILABLE") ?
                                      ORCA_MODEL_UNSUPPORTED :
                   (e.code == "IO_ERROR" || e.code == "FILE_LIMIT") ? ORCA_MODEL_IO :
                                                                      ORCA_MODEL_INVALID;
        response = {{"ok", false}, {"schemaVersion", 1}, {"error", {{"code", e.code}, {"message", e.what()}}}};
    } catch (const nlohmann::json::exception& e) {
        code     = ORCA_MODEL_INVALID;
        response = {{"ok", false}, {"schemaVersion", 1}, {"error", {{"code", "INVALID_JSON"}, {"message", e.what()}}}};
    } catch (const std::exception& e) {
        code     = ORCA_MODEL_INTERNAL;
        response = {{"ok", false}, {"schemaVersion", 1}, {"error", {{"code", "SDK_ERROR"}, {"message", e.what()}}}};
    } catch (...) {
        code     = ORCA_MODEL_INTERNAL;
        response = {{"ok", false}, {"schemaVersion", 1}, {"error", {{"code", "SDK_ERROR"}, {"message", "Unknown SDK failure"}}}};
    }
    try {
        *result = allocated(response.dump());
    } catch (...) {
        return ORCA_MODEL_INTERNAL;
    }
    return *result ? code : ORCA_MODEL_INTERNAL;
}
void orca_model_cancel(orca_model_context* p)
{
    if (auto c = acquire(p)) {
        c->cancel = true;
        if (auto b = std::atomic_load(&c->backend))
            b->cancel(b->context);
    }
}
void orca_model_destroy(orca_model_context* p)
{
    std::shared_ptr<Context> c;
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto i = registry.find(reinterpret_cast<uintptr_t>(p));
        if (i == registry.end())
            return;
        c = std::move(i->second);
        registry.erase(i);
    }
    c->cancel = true;
    if (auto backend = std::atomic_load(&c->backend))
        backend->cancel(backend->context);
}
void orca_model_free_string(char* p) { std::free(p); }
int orca_model_set_platform_transport(orca_model_context* p, orca_model_platform_request_fn transport, void* user)
{
    auto c = acquire(p);
    if (!c)
        return ORCA_MODEL_INVALID;
    std::unique_lock<std::mutex> lock(c->mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return ORCA_MODEL_BUSY;
    c->transport      = transport;
    c->transport_user = user;
    return ORCA_MODEL_OK;
}
int orca_model_set_slicing_backend(orca_model_context* p, const char* library)
{
    auto c = acquire(p);
    if (!c || !library)
        return ORCA_MODEL_INVALID;
    std::unique_lock<std::mutex> lock(c->mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return ORCA_MODEL_BUSY;
    try {
        auto filename = F::u8path(library);
        if (!filename.is_absolute() || !F::is_regular_file(filename))
            return ORCA_MODEL_IO;
        auto backend = std::make_shared<Backend>();
#ifdef _WIN32
        backend->library = retained_module(filename);
        auto symbol      = [&](const char* name) {
            return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(backend->library), name));
        };
#else
        backend->library = retained_module(filename);
        auto symbol      = [&](const char* name) { return dlsym(backend->library, name); };
#endif
        if (!backend->library)
            return ORCA_MODEL_IO;
        auto create          = reinterpret_cast<decltype(&orca_mobile_slicing_create)>(symbol("orca_mobile_slicing_create"));
        backend->execute     = reinterpret_cast<Backend::Execute>(symbol("orca_mobile_slicing_execute"));
        backend->cancel      = reinterpret_cast<decltype(backend->cancel)>(symbol("orca_mobile_slicing_cancel"));
        backend->destroy     = reinterpret_cast<decltype(backend->destroy)>(symbol("orca_mobile_slicing_destroy"));
        backend->free_string = reinterpret_cast<decltype(backend->free_string)>(symbol("orca_mobile_slicing_free_string"));
        if (!create || !backend->execute || !backend->cancel || !backend->destroy || !backend->free_string)
            return ORCA_MODEL_UNSUPPORTED;
        backend->context = create(c->root.u8string().c_str());
        if (!backend->context)
            return ORCA_MODEL_INTERNAL;
        std::atomic_store(&c->backend, std::move(backend));
        return ORCA_MODEL_OK;
    } catch (...) {
        return ORCA_MODEL_INTERNAL;
    }
}
}

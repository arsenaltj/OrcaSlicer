#include "orca/model/model_sdk_c.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
static int run(int argc, char** argv)
{
    try {
        if (argc % 2 == 0)
            throw std::runtime_error("Every option requires a value");
        std::string workspace, request, output, slicing_backend;
        for (int i = 1; i + 1 < argc; i += 2) {
            std::string a = argv[i];
            if (a == "--workspace")
                workspace = argv[i + 1];
            else if (a == "--request")
                request = argv[i + 1];
            else if (a == "--output")
                output = argv[i + 1];
            else if (a == "--slicing-backend")
                slicing_backend = argv[i + 1];
            else
                throw std::runtime_error("Unknown argument");
        }
        if (workspace.empty() || request.empty() || output.empty())
            throw std::runtime_error("Usage: --workspace DIR --request FILE --output FILE");
        namespace f = std::filesystem;
        using J     = nlohmann::json;
        auto root = f::weakly_canonical(f::absolute(f::u8path(workspace))), dest = f::weakly_canonical(f::absolute(f::u8path(output)));
        auto rel = dest.lexically_relative(root);
        if (rel.empty() || rel.is_absolute() || *rel.begin() == "..")
            throw std::runtime_error("Result must remain in workspace");
        if (f::file_size(f::u8path(request)) > 64u * 1024u * 1024u)
            throw std::runtime_error("Request limit");
        std::ifstream input(f::u8path(request));
        J commands;
        input >> commands;
        auto* context = orca_model_create(workspace.c_str());
        if (!context)
            throw std::runtime_error("Workspace unavailable");
        struct Owner
        {
            orca_model_context* p;
            ~Owner() { orca_model_destroy(p); }
        } owner{context};
        // This is a fixed sibling from the immutable bundle, never a "latest"
        // build lookup or a path supplied by an untrusted operation JSON.
        if (slicing_backend.empty()) {
#ifdef _WIN32
            auto sibling = f::absolute(f::u8path(argv[0])).parent_path() / "orca_mobile_slicing_sdk_c.dll";
#else
            auto sibling = f::absolute(f::u8path(argv[0])).parent_path() / "liborca_mobile_slicing_sdk_c.so";
#endif
            if (f::is_regular_file(sibling))
                slicing_backend = sibling.u8string();
        }
        if (!slicing_backend.empty() && orca_model_set_slicing_backend(context, slicing_backend.c_str()) != ORCA_MODEL_OK)
            throw std::runtime_error("Verified Slicing SDK backend could not be loaded");
        auto operations = commands.contains("operations") ? commands.at("operations") : J::array({commands});
        J results       = J::array();
        bool ok         = true;
        if (!operations.is_array() || operations.empty() || operations.size() > 128)
            throw std::runtime_error("Invalid operation batch");
        for (const auto& r : operations) {
            char* text = nullptr;
            auto code  = orca_model_execute(context, r.dump().c_str(), nullptr, nullptr, &text);
            if (!text)
                throw std::runtime_error("SDK did not return result");
            J result;
            try {
                result = J::parse(text);
            } catch (...) {
                orca_model_free_string(text);
                throw;
            }
            orca_model_free_string(text);
            results.push_back(result);
            if (code) {
                ok = false;
                break;
            }
        }
        auto response = commands.contains("operations") ? J{{"ok", ok}, {"schemaVersion", 1}, {"results", results}} : results.at(0);
        f::create_directories(dest.parent_path());
        std::ofstream out(dest, std::ios::binary);
        out << response.dump();
        if (!out)
            throw std::runtime_error("Cannot write result");
        return ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** wide_argv)
{
    // Windows CRT narrow argv uses the active codepage, not UTF-8. Keep the
    // public SDK path contract valid for Chinese and other Unicode workspaces.
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i)
        arguments.push_back(std::filesystem::path(wide_argv[i]).u8string());
    std::vector<char*> pointers;
    for (auto& argument : arguments)
        pointers.push_back(argument.data());
    return run(argc, pointers.data());
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif

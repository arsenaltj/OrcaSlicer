#include "model_internal.hpp"

namespace orca_model_detail {
J manufacturing_bridge(Context& context, const J& request)
{
    expected(context, request);
    const auto operation = request.at("operation").get<std::string>();
    J native             = request.value("request", J::object());
    if (!native.is_object())
        throw Error("INVALID_REQUEST", "Native configuration must be an object");
    // This bridge snapshots the authoritative document. A separate writable
    // native command history would invalidate the Model revision binding.
    if (native.contains("projectCommands") || native.contains("command") || native.contains("transforms") || native.contains("snapshotPath"))
        throw Error("DOCUMENT_BRIDGE_COMMAND", "Edit the document before creating its manufacturing snapshot");
    const auto exported       = execute(context, J{{"schemaVersion", 1},
                                                   {"operation", "exportManufacturing"},
                                                   {"expectedRevision", context.doc.revision},
                                                   {"format", "obj"}});
    const auto& mesh          = exported.at("artifacts").at(0);
    const auto& appearance    = exported.at("artifacts").at(1);
    native["schemaVersion"]   = 2;
    native["operation"]       = operation == "recommendDocument" ? "recommend" : operation == "sliceDocument" ? "slice" : "import";
    native["inputPath"]       = mesh.at("path");
    native["inputSha256"]     = mesh.at("sha256");
    native["projectId"]       = context.doc.id;
    native["projectRevision"] = context.doc.revision;
    native["projectCommands"] = J::array({{{"type", "applyAppearance"},
                                           {"objectIndex", 0},
                                           {"volumeIndex", 0},
                                           {"appearancePath", appearance.at("path")},
                                           {"appearanceSha256", appearance.at("sha256")}}});
    native["outputDir"]       = output(context, "manufacturing/" + context.doc.id + "/r" + std::to_string(context.doc.revision)).u8string();
    auto binding              = J{{"schemaVersion", 1},
                                  {"documentId", context.doc.id},
                                  {"modelVersion", exported.at("modelVersion")},
                                  {"revision", context.doc.revision},
                                  {"geometryRevision", context.doc.geometry},
                                  {"inputSha256", context.doc.input_sha},
                                  {"units", "mm"},
                                  {"upAxis", "Z"},
                                  {"manufacturingGeometrySha256", mesh.at("sha256")},
                                  {"appearanceSha256", appearance.at("sha256")},
                                  {"artifacts", exported.at("artifacts")},
                                  {"nativeRequest", native}};
    auto snapshot             = output(context, "manufacturing/" + context.doc.id + "/r" + std::to_string(context.doc.revision) + "-" +
                                                    sha256(binding.dump()).substr(0, 16) + ".binding.json");
    atomic_write(snapshot, binding.dump());
    binding["snapshotPath"]   = snapshot.u8string();
    binding["snapshotSha256"] = sha256(read(snapshot));
    if (operation == "manufacturingSnapshot")
        return binding;
    const auto result = execute(context, J{{"schemaVersion", 1}, {"operation", "slicing"}, {"request", native}});
    check_cancel(context);
    binding["nativeResult"] = result;
    return binding;
}
} // namespace orca_model_detail

#include "model_internal.hpp"
namespace orca_model_detail {
J execute(Context& c, const J& r)
{
    if (r.value("schemaVersion", 0) != 1)
        throw Error("SCHEMA_VERSION", "Expected schemaVersion=1");
    auto op = r.at("operation").get<std::string>();
    if (op == "capabilities")
        return J::parse(orca_model_capabilities());
    if (op == "manufacturingSnapshot" || op == "sliceDocument" || op == "recommendDocument")
        return manufacturing_bridge(c, r);
    if (op == "platformAI") {
        if (!c.transport)
            throw Error("PLATFORM_UNAVAILABLE", "Host platform transport is not configured");
        auto command       = r.at("command").get<std::string>();
        std::string method = "GET", route = "/v1/ai/";
        if (command == "convertModel") {
            method = "POST";
            route  = "/v1/model-conversions";
        } else if (command == "queryConversion") {
            auto id = r.at("conversionId").get<std::string>();
            if (id.empty() || id.size() > 100 ||
                id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
                throw Error("INVALID_CONVERSION", "Invalid conversion ID");
            route = "/v1/model-conversions/" + id;
        } else if (command == "capabilities" || command == "models")
            route += command;
        else if (command == "quote") {
            method = "POST";
            route += "quotes";
        } else if (command == "submit") {
            method = "POST";
            route += "jobs";
        } else if (command == "query" || command == "cancel" || command == "reconcile") {
            auto id = r.at("jobId").get<std::string>();
            if (id.empty() || id.size() > 100 ||
                id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
                throw Error("INVALID_JOB", "Invalid job ID");
            route += "jobs/" + id;
            if (command != "query") {
                method = "POST";
                route += "/" + command;
            }
        } else
            throw Error("INVALID_COMMAND", "Unknown platform AI command");
        auto body            = r.value("body", J::object()).dump();
        const char* response = nullptr;
        int status           = c.transport(method.c_str(), route.c_str(), body.c_str(), c.transport_user, &response);
        check_cancel(c);
        if (!response || std::strlen(response) > max_bytes)
            throw Error("PLATFORM_RESPONSE", "Missing or excessive platform response");
        auto data = J::parse(response);
        if (status < 200 || status >= 300)
            throw Error("PLATFORM_HTTP_ERROR", data.dump());
        return J{{"httpStatus", status}, {"data", std::move(data)}};
    }
    if (op == "slicing") {
        if (!c.backend)
            throw Error("SLICING_UNAVAILABLE", "Host must configure a verified Slicing SDK bundle");
        auto request = r.at("request").dump();
        char* raw    = nullptr;
        auto code    = c.backend->execute(c.backend->context, request.c_str(), c.event, c.event_user, &raw);
        if (!raw)
            throw Error("SLICING_FAILED", "Backend did not return a result");
        J response;
        try {
            response = J::parse(raw);
        } catch (...) {
            c.backend->free_string(raw);
            throw;
        }
        c.backend->free_string(raw);
        if (code == ORCA_MOBILE_SLICING_CANCELED)
            throw Error("CANCELED", "Native slicing canceled");
        if (code || !response.value("ok", false))
            throw Error("SLICING_FAILED", response.dump());
        return response;
    }
    if (op == "import" || op == "inspect" || op == "importConverted" || op == "create" || op == "load")
        return open_document(c, r);
    if (op == "getScene")
        return scene_descriptor(c, c.doc);
    if (op == "selectRegion") {
#ifdef ORCA_MODEL_HAS_REGION_CORE
        return orca_model_select_region(c.doc, r, c.cancel);
#else
        throw Error("REGION_CORE_UNAVAILABLE", "Build with the existing native region algorithm enabled");
#endif
    }
    if (op == "analyze") {
        scene(c.doc, false, false);
        std::map<std::pair<uint32_t, uint32_t>, size_t> edges;
        size_t degenerate = 0;
        for (auto t : c.doc.mesh.faces) {
            check_cancel(c);
            if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2])
                ++degenerate;
            for (int i = 0; i < 3; ++i) {
                auto a = t[i], b = t[(i + 1) % 3];
                if (a > b)
                    std::swap(a, b);
                ++edges[{a, b}];
            }
        }
        size_t boundary = 0, nonmanifold = 0;
        for (auto& e : edges) {
            if (e.second == 1)
                ++boundary;
            else if (e.second > 2)
                ++nonmanifold;
        }
        return J{{"documentId", c.doc.id},         {"revision", c.doc.revision},        {"geometryRevision", c.doc.geometry},
                 {"boundaryEdges", boundary},      {"nonManifoldEdges", nonmanifold},   {"degenerateFaces", degenerate},
                 {"scope", "index-topology-only"}, {"selfIntersectionsChecked", false}, {"printabilityVerified", false}};
    }
    if (op == "pick") {
        scene(c.doc, false, false);
        V origin = r.at("origin").get<V>(), direction = r.at("direction").get<V>();
        for (auto n : origin)
            if (!std::isfinite(n))
                throw Error("INVALID_RAY", "Invalid ray");
        double norm = 0;
        for (auto n : direction) {
            if (!std::isfinite(n))
                throw Error("INVALID_RAY", "Invalid ray");
            norm += n * n;
        }
        if (norm < 1e-20)
            throw Error("INVALID_RAY", "Ray direction is zero");
        auto sub       = [](V a, V b) { return V{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
        auto cross     = [](V a, V b) { return V{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
        auto dot       = [](V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        double nearest = 1e300;
        size_t face    = 0;
        bool hit       = false;
        for (size_t i = 0; i < c.doc.mesh.faces.size(); ++i) {
            check_cancel(c);
            auto t = c.doc.mesh.faces[i];
            auto a = c.doc.mesh.vertices[t[0]], e1 = sub(c.doc.mesh.vertices[t[1]], a), e2 = sub(c.doc.mesh.vertices[t[2]], a),
                 p     = cross(direction, e2);
            double det = dot(e1, p);
            if (std::abs(det) < 1e-12)
                continue;
            auto s   = sub(origin, a);
            double u = dot(s, p) / det;
            if (u < 0 || u > 1)
                continue;
            auto q   = cross(s, e1);
            double v = dot(direction, q) / det;
            if (v < 0 || u + v > 1)
                continue;
            double distance = dot(e2, q) / det;
            if (distance > 0 && distance < nearest) {
                nearest = distance;
                face    = i;
                hit     = true;
            }
        }
        J answer = {{"documentId", c.doc.id}, {"revision", c.doc.revision}, {"geometryRevision", c.doc.geometry}, {"hit", hit}};
        if (hit) {
            answer["faceIndex"] = face;
            answer["position"]  = V{origin[0] + nearest * direction[0], origin[1] + nearest * direction[1],
                                   origin[2] + nearest * direction[2]};
        }
        return answer;
    }
    expected(c, r);
    if (op == "prepareCandidate") {
        auto command = r.at("command");
        auto name    = command.value("operation", std::string());
        if (name != "transform" && name != "paint" && name != "assignMaterial" && name != "weldExact")
            throw Error("INVALID_CANDIDATE", "Unsupported candidate command");
        Context isolated;
        isolated.root               = c.root;
        isolated.doc                = c.doc;
        isolated.return_inline      = true;
        command["schemaVersion"]    = 1;
        command["expectedRevision"] = c.doc.revision;
        execute(isolated, command);
        check_cancel(c);
        auto descriptor         = preview_document(c, isolated.doc);
        const auto candidate_id = c.doc.id + ":candidate:" + std::to_string(c.generation) + ":" + std::to_string(c.doc.revision) + ":" +
                                  sha256(command.dump()).substr(0, 24);
        J response{{"candidateId", candidate_id},
                   {"sourceRevision", c.doc.revision},
                   {"sourceGeneration", c.generation},
                   {"scene", std::move(descriptor)}};
        check_cancel(c);
        c.candidate            = std::move(isolated.doc);
        c.candidate_base       = c.doc.revision;
        c.candidate_id         = candidate_id;
        c.candidate_document   = c.doc.id;
        c.candidate_generation = c.generation;
        c.candidate_geometry   = c.doc.geometry;
        return response;
    }
    if (op == "acceptCandidate") {
        if (c.candidate.id.empty() || c.candidate_base != c.doc.revision || c.candidate_document != c.doc.id ||
            c.candidate_generation != c.generation || c.candidate_geometry != c.doc.geometry ||
            r.at("candidateId").get<std::string>() != c.candidate_id)
            throw Error("STALE_CANDIDATE", "Candidate does not match current source");
        auto next = c.candidate;
        return commit(c, std::move(next));
    }
    if (op == "discardCandidate") {
        auto descriptor = preview_document(c, c.doc);
        clear_candidate(c);
        return descriptor;
    }
    if (op == "save") {
        auto descriptor = preview_document(c, c.doc);
        auto path       = output(c, "documents/" + c.doc.id + ".json", true);
        auto stored     = scene(c.doc);
        stored["protectedSources"].erase(path.u8string());
        const auto bytes                   = stored.dump();
        auto protected_sources             = c.protected_sources;
        protected_sources[path.u8string()] = sha256(bytes);
        check_cancel(c);
        atomic_write(path, bytes);
        c.protected_sources.swap(protected_sources);
        return descriptor;
    }
    if (op == "undo" || op == "redo") {
        auto& from = op == "undo" ? c.undo : c.redo;
        auto& to   = op == "undo" ? c.redo : c.undo;
        if (from.empty())
            throw Error("NO_HISTORY", "No history available");
        auto previous     = from.back();
        previous.revision = c.doc.revision + 1;
        auto descriptor   = preview_document(c, previous);
        auto destination  = to;
        destination.push_back(c.doc);
        check_cancel(c);
        from.pop_back();
        to.swap(destination);
        c.doc = std::move(previous);
        clear_candidate(c);
        return descriptor;
    }
    if (op == "exportManufacturing") {
        auto format = r.value("format", std::string("obj"));
        if (format != "obj" && format != "stl")
            throw Error("UNSUPPORTED_FORMAT", "Supported export: OBJ/STL");
        bool varied = false;
        for (size_t i = 1; i < c.doc.mesh.materials.size(); ++i)
            if (c.doc.mesh.materials[i] != c.doc.mesh.materials[0])
                varied = true;
        if (format == "stl" && !r.value("acceptAppearanceLoss", false))
            throw Error("APPEARANCE_CONFIRMATION", "STL drops appearance; explicit confirmation required");
        std::ostringstream out;
        out << std::setprecision(17);
        if (format == "obj") {
            for (auto& v : c.doc.mesh.vertices)
                out << "v " << v[0] << ' ' << v[1] << ' ' << v[2] << '\n';
            for (auto& t : c.doc.mesh.faces)
                out << "f " << t[0] + 1 << ' ' << t[1] + 1 << ' ' << t[2] + 1 << '\n';
        } else {
            out << "solid model\n";
            for (auto& t : c.doc.mesh.faces) {
                out << "facet normal 0 0 0\nouter loop\n";
                for (auto i : t) {
                    const auto& v = c.doc.mesh.vertices[i];
                    out << "vertex " << v[0] << ' ' << v[1] << ' ' << v[2] << '\n';
                }
                out << "endloop\nendfacet\n";
            }
            out << "endsolid model\n";
        }
        auto bytes                = out.str();
        auto p                    = output(c, r.value("outputPath", "exports/" + c.doc.id + "/r" + std::to_string(c.doc.revision) + "-" +
                                                                        sha256(bytes).substr(0, 16) + "." + format));
        const auto namespace_path = F::weakly_canonical(p).lexically_relative(F::weakly_canonical(c.root));
        if (namespace_path.begin() != namespace_path.end() &&
            (*namespace_path.begin() == "documents" || *namespace_path.begin() == "scenes" || *namespace_path.begin() == "manufacturing"))
            throw Error("SOURCE_OVERWRITE", "Manufacturing exports cannot overwrite SDK document or scene assets");
        check_cancel(c);
        atomic_write(p, bytes);
        auto appearance        = output(c, p.u8string() + ".appearance.json");
        auto data              = scene(c.doc, false);
        data["faceColors"]     = c.doc.mesh.colors;
        data["materialIds"]    = c.doc.mesh.materials;
        data["schemaVersion"]  = 1;
        data["geometrySha256"] = sha256(bytes);
        data.erase("vertices");
        data.erase("triangles");
        atomic_write(appearance, data.dump());
        return J{{"documentId", c.doc.id},
                 {"modelVersion", c.doc.id + ":" + std::to_string(c.doc.revision)},
                 {"revision", c.doc.revision},
                 {"inputSha256", c.doc.input_sha},
                 {"units", "mm"},
                 {"upAxis", "Z"},
                 {"multiMaterial", varied},
                 {"materialRoutingVerified", false},
                 {"artifacts", J::array({{{"kind", "manufacturingMesh"},
                                          {"path", p.u8string()},
                                          {"format", format},
                                          {"sha256", sha256(bytes)},
                                          {"sizeBytes", bytes.size()}},
                                         {{"kind", "appearance"},
                                          {"path", appearance.u8string()},
                                          {"format", "json"},
                                          {"sha256", sha256(data.dump())},
                                          {"sizeBytes", data.dump().size()}}})}};
    }
    Document next = c.doc;
    if (op == "transform") {
        V translation = r.value("translation", V{0, 0, 0}), scale = r.value("scale", V{1, 1, 1}),
          rotation = r.value("rotationDegrees", V{0, 0, 0});
        for (auto n : scale)
            if (!std::isfinite(n) || n <= 0)
                throw Error("INVALID_SCALE", "Scale must be finite and positive");
        size_t first_vertex = 0, vertex_count = next.mesh.vertices.size();
        if (r.contains("entityId")) {
            bool found = false;
            if (!next.appearance.contains("parts"))
                throw Error("UNKNOWN_ENTITY", "Document has no verified entity ranges");
            for (const auto& part : next.appearance.at("parts"))
                if (part.at("entityId") == r.at("entityId")) {
                    first_vertex = part.at("firstVertex").get<size_t>();
                    vertex_count = part.at("vertexCount").get<size_t>();
                    found        = true;
                    break;
                }
            if (!found)
                throw Error("UNKNOWN_ENTITY", "Entity is not present in the verified geometry ranges");
        }
        if (first_vertex > next.mesh.vertices.size() || vertex_count > next.mesh.vertices.size() - first_vertex)
            throw Error("INVALID_PART_RANGES", "Entity vertex range exceeds geometry");
        for (size_t vertex = first_vertex; vertex < first_vertex + vertex_count; ++vertex) {
            auto& v = next.mesh.vertices[vertex];
            check_cancel(c);
            for (int i = 0; i < 3; ++i)
                v[i] *= scale[i];
            for (int axis = 0; axis < 3; ++axis) {
                int a = (axis + 1) % 3, b = (axis + 2) % 3;
                auto angle = rotation[axis] * 3.14159265358979323846 / 180;
                auto x = v[a], y = v[b];
                v[a] = x * std::cos(angle) - y * std::sin(angle);
                v[b] = x * std::sin(angle) + y * std::cos(angle);
            }
            for (int i = 0; i < 3; ++i)
                v[i] += translation[i];
        }
        next.geometry++;
    } else if (op == "paint" || op == "assignMaterial") {
        auto indices = r.at("faces").get<std::vector<size_t>>();
        if (indices.empty())
            throw Error("EMPTY_SELECTION", "Select faces");
        auto color = r.value("color", std::array<double, 4>{0.8, 0.8, 0.8, 1});
        for (auto n : color)
            if (!std::isfinite(n) || n < 0 || n > 1)
                throw Error("INVALID_COLOR", "Color must be normalized RGBA");
        auto material = r.value("materialId", 0);
        if (material < 0 || material > 255)
            throw Error("INVALID_MATERIAL", "Material ID must be 0..255");
        for (auto i : indices) {
            check_cancel(c);
            if (i >= next.mesh.faces.size())
                throw Error("INVALID_FACE", "Face out of range");
            if (op == "paint")
                next.mesh.colors[i] = color;
            else
                next.mesh.materials[i] = material;
        }
    } else if (op == "weldExact") {
        std::map<V, uint32_t> unique;
        std::vector<uint32_t> mapping;
        std::vector<V> vertices;
        for (auto v : next.mesh.vertices) {
            check_cancel(c);
            auto found = unique.find(v);
            if (found == unique.end()) {
                auto n = uint32_t(vertices.size());
                vertices.push_back(v);
                unique.emplace(v, n);
                mapping.push_back(n);
            } else
                mapping.push_back(found->second);
        }
        std::set<T> seen;
        Mesh repaired;
        repaired.vertices = std::move(vertices);
        for (size_t i = 0; i < next.mesh.faces.size(); ++i) {
            auto t = next.mesh.faces[i];
            for (auto& index : t)
                index = mapping[index];
            if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2])
                continue;
            auto key = t;
            std::sort(key.begin(), key.end());
            if (!seen.insert(key).second)
                continue;
            repaired.faces.push_back(t);
            repaired.colors.push_back(next.mesh.colors[i]);
            repaired.materials.push_back(next.mesh.materials[i]);
        }
        next.mesh = std::move(repaired);
        if (next.appearance.contains("parts")) {
            next.appearance["sourcePartsBeforeTopologyChange"] = next.appearance.at("parts");
            next.appearance.erase("parts");
            next.appearance["partMappingInvalidatedByTopologyChange"] = true;
        }
        next.geometry++;
    } else
        throw Error("UNSUPPORTED_OPERATION", "Operation is not implemented by this SDK version");
    return commit(c, std::move(next));
}
} // namespace orca_model_detail

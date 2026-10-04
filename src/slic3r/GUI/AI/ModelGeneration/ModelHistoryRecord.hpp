#pragma once
#include <nlohmann/json.hpp>
#include <charconv>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

// Keep ordinary fields in the vendored library's original DOM adapter;
// only the two large root states use canonical encoded containers. This
// adapter is private to history decoding and covered when the library changes.
class ModelHistoryRecord : public nlohmann::json_sax<nlohmann::json> {
    using Json = nlohmann::json;
    struct Frame {
        bool object, first = true;
        std::string key, array = "[";
        std::map<std::string, std::string> members;
        explicit Frame(bool is_object) : object(is_object) {}
    };
    Json m_fields;
    nlohmann::detail::json_sax_dom_parser<Json> dom;
    std::vector<Frame> stack;
    std::map<std::string, std::string> encoded;
    std::function<bool()> canceled;
    std::string root_key;
    size_t depth = 0, events = 0;
    bool freezing = false;
    bool checkpoint() { return !canceled || (++events % 256 != 0) || !canceled(); }
    bool put(std::string bytes) {
        if (stack.empty()) {
            encoded[root_key] = std::move(bytes);
            freezing = false;
        } else {
            auto& frame = stack.back();
            if (frame.object) frame.members[frame.key] = std::move(bytes);
            else {
                if (!frame.first) frame.array += ',';
                frame.first = false;
                frame.array += bytes;
            }
        }
        return true;
    }
    template<class Integer> bool integer(Integer value) {
        char buffer[32];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return result.ec == std::errc{} && put(std::string(buffer, result.ptr));
    }
    bool end_frozen(bool object) {
        if (stack.empty() || stack.back().object != object) return false;
        auto frame = std::move(stack.back());
        stack.pop_back();
        if (!object) { frame.array += ']'; return put(std::move(frame.array)); }
        std::string bytes = "{";
        bool first = true;
        for (const auto& member : frame.members) {
            if (!first) bytes += ',';
            first = false;
            bytes += Json(member.first).dump() + ':' + member.second;
        }
        bytes += '}';
        return put(std::move(bytes));
    }
public:
    explicit ModelHistoryRecord(std::function<bool()> stop = {}) : dom(m_fields, false), canceled(std::move(stop)) {}
    static bool parse(const std::string& contents, Json& fields,
                      std::map<std::string, std::string>& frozen,
                      const std::function<bool()>& stop = {}) {
        if (stop && stop()) return false;
        if (contents.size() < 64 * 1024) {
            auto document = Json::parse(contents, nullptr, false);
            if (!document.is_object() || (stop && stop())) return false;
            std::map<std::string, std::string> large;
            for (const char* key : {"beauty_workbench", "beauty_puzzle_draft"}) {
                const auto field = document.find(key);
                if (field == document.end()) continue;
                large.emplace(key, field->dump());
                document.erase(field);
                if (stop && stop()) return false;
            }
            fields = std::move(document);
            frozen = std::move(large);
            return true;
        }
        ModelHistoryRecord parser(stop);
        if (!Json::sax_parse(contents, &parser) || !parser.m_fields.is_object() || (stop && stop())) return false;
        fields = std::move(parser.m_fields);
        frozen = std::move(parser.encoded);
        return true;
    }
    bool null() override { return checkpoint() && (freezing ? put("null") : dom.null()); }
    bool boolean(bool value) override { return checkpoint() && (freezing ? put(value ? "true" : "false") : dom.boolean(value)); }
    bool number_integer(number_integer_t value) override { return checkpoint() && (freezing ? integer(value) : dom.number_integer(value)); }
    bool number_unsigned(number_unsigned_t value) override { return checkpoint() && (freezing ? integer(value) : dom.number_unsigned(value)); }
    bool number_float(number_float_t value, const string_t& token) override {
        return checkpoint() && (freezing ? put(Json(value).dump()) : dom.number_float(value, token));
    }
    bool string(string_t& value) override { return checkpoint() && (freezing ? put(Json(value).dump()) : dom.string(value)); }
    bool binary(binary_t&) override { return false; }
    bool start_object(size_t elements) override {
        if (!checkpoint()) return false;
        ++depth;
        if (freezing) { stack.emplace_back(true); return true; }
        return dom.start_object(elements);
    }
    bool key(string_t& value) override {
        if (!checkpoint()) return false;
        if (depth == 1 && (value == "beauty_workbench" || value == "beauty_puzzle_draft")) {
            root_key = std::move(value); freezing = true; return true;
        }
        if (freezing) { stack.back().key = std::move(value); return true; }
        return dom.key(value);
    }
    bool end_object() override {
        if (!checkpoint() || depth == 0) return false;
        --depth;
        return freezing ? end_frozen(true) : dom.end_object();
    }
    bool start_array(size_t elements) override {
        if (!checkpoint()) return false;
        ++depth;
        if (freezing) { stack.emplace_back(false); return true; }
        return dom.start_array(elements);
    }
    bool end_array() override {
        if (!checkpoint() || depth == 0) return false;
        --depth;
        return freezing ? end_frozen(false) : dom.end_array();
    }
    bool parse_error(size_t, const std::string&, const Json::exception&) override { return false; }
};

} // namespace Slic3r::GUI

#pragma once

#include "slic3r/AI/SmartSlicing/Ports/ITrialSliceExecutor.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>

namespace Slic3r::AI::SmartSlicing {

// Reuses only completed trial metrics. Candidate status, diagnostics, and attached metrics are
// deliberately excluded from the key because they are workflow output rather than slicing input.
class CachingTrialSliceExecutor final : public ITrialSliceExecutor
{
public:
    explicit CachingTrialSliceExecutor(ITrialSliceExecutor& delegate, size_t maximum_entries = 16)
        : m_delegate(delegate), m_maximum_entries(maximum_entries)
    {}

    TrialSliceResult execute_trial_slice(const SliceCandidate& candidate) override
    {
        if (m_maximum_entries == 0)
            return execute_delegate(candidate);

        const std::string key = candidate_key(candidate);
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_results.find(key);
            if (found != m_results.end())
                return found->second;
        }

        TrialSliceResult result = execute_delegate(candidate);
        if (result.status != TrialSliceStatus::Succeeded || !result.metrics ||
            !result.metrics->has_valid_measurements() ||
            result.candidate_id != candidate.id || result.base_revision != candidate.base_revision)
            return result;

        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_results.find(key) != m_results.end())
            return result;
        while (m_results.size() >= m_maximum_entries) {
            m_results.erase(m_insertion_order.front());
            m_insertion_order.pop_front();
        }
        m_insertion_order.push_back(key);
        m_results.emplace(key, result);
        return result;
    }

    void cancel_trial_slice() override
    {
        try {
            m_delegate.cancel_trial_slice();
        } catch (...) {
            // Cancellation is best effort. The coordinator must still finish terminal cleanup.
        }
    }

private:
    TrialSliceResult execute_delegate(const SliceCandidate& candidate)
    {
        try {
            return m_delegate.execute_trial_slice(candidate);
        } catch (...) {
            TrialSliceResult result;
            result.candidate_id    = candidate.id;
            result.base_revision   = candidate.base_revision;
            result.status          = TrialSliceStatus::Failed;
            result.diagnostic_code = "trial_slice_executor_exception";
            return result;
        }
    }

    static void append_uint64(std::string& key, uint64_t value)
    {
        for (size_t index = 0; index < sizeof(value); ++index) {
            key.push_back(static_cast<char>(value & 0xff));
            value >>= 8;
        }
    }

    static void append_bool(std::string& key, bool value) { key.push_back(value ? '\x01' : '\x00'); }

    static void append_double(std::string& key, double value)
    {
        static_assert(sizeof(double) == sizeof(uint64_t));
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        append_uint64(key, bits);
    }

    static void append_string(std::string& key, const std::string& value)
    {
        append_uint64(key, static_cast<uint64_t>(value.size()));
        key.append(value);
    }

    template<class Enum> static void append_enum(std::string& key, Enum value)
    {
        static_assert(std::is_enum_v<Enum>);
        append_uint64(key, static_cast<uint64_t>(value));
    }

    static void append_config_value(std::string& key, const ConfigValue& value)
    {
        append_uint64(key, static_cast<uint64_t>(value.index()));
        std::visit(
            [&key](const auto& typed_value) {
                using Value = std::decay_t<decltype(typed_value)>;
                if constexpr (std::is_same_v<Value, bool>)
                    append_bool(key, typed_value);
                else if constexpr (std::is_same_v<Value, int64_t>)
                    append_uint64(key, static_cast<uint64_t>(typed_value));
                else if constexpr (std::is_same_v<Value, double>)
                    append_double(key, typed_value);
                else
                    append_string(key, typed_value);
            },
            value);
    }

    static std::string candidate_key(const SliceCandidate& candidate)
    {
        std::string key;
        key.reserve(256);
        append_string(key, candidate.id);
        append_uint64(key, candidate.base_revision.model_revision);
        append_uint64(key, candidate.base_revision.config_revision);
        append_uint64(key, candidate.base_revision.plate_revision);
        append_string(key, candidate.base_revision.fingerprint);
        append_enum(key, candidate.goal);

        append_bool(key, candidate.repair.has_value());
        if (candidate.repair) {
            append_uint64(key, static_cast<uint64_t>(candidate.repair->operation_codes.size()));
            for (const std::string& operation_code : candidate.repair->operation_codes)
                append_string(key, operation_code);
            append_bool(key, candidate.repair->changes_geometry_semantics);
        }

        append_uint64(key, static_cast<uint64_t>(candidate.placement.transforms.size()));
        for (const ObjectTransform& transform : candidate.placement.transforms) {
            append_uint64(key, transform.object_id);
            append_uint64(key, transform.instance_id);
            for (double component : transform.matrix)
                append_double(key, component);
        }

        append_uint64(key, static_cast<uint64_t>(candidate.parameters.entries.size()));
        for (const ConfigPatchEntry& entry : candidate.parameters.entries) {
            append_enum(key, entry.scope);
            append_enum(key, entry.owner);
            append_uint64(key, static_cast<uint64_t>(entry.target_id));
            append_string(key, entry.key);
            append_config_value(key, entry.expected_value);
            append_config_value(key, entry.new_value);
            append_string(key, entry.reason_code);
        }
        append_uint64(key, static_cast<uint64_t>(candidate.parameters.explanation_codes.size()));
        for (const std::string& explanation_code : candidate.parameters.explanation_codes)
            append_string(key, explanation_code);
        append_string(key, candidate.explanation);
        return key;
    }

    ITrialSliceExecutor& m_delegate;
    const size_t m_maximum_entries;
    std::mutex m_mutex;
    std::unordered_map<std::string, TrialSliceResult> m_results;
    std::deque<std::string> m_insertion_order;
};

} // namespace Slic3r::AI::SmartSlicing

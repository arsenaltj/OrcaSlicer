#pragma once

namespace Slic3r {

// The short value preserves OrcaSlicer's existing UI while the full value is
// available for exact support correlation. Both are generated from one source
// identity without changing target-wide compiler definitions.
const char *build_commit_hash() noexcept;
const char *build_source_commit() noexcept;
const wchar_t *build_commit_hash_wide() noexcept;

} // namespace Slic3r

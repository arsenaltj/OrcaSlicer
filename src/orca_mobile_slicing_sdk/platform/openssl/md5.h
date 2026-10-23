#pragma once

#ifndef ORCA_MOBILE_ANDROID
#error "The mobile MD5 facade is only available to the Android core build"
#endif

// Build-private MD5 compatibility for the pinned Android dependency graph.
// Digest state and standard byte serialization are supplied by the SDK adapter.
#include "../MobileDigest.hpp"

#ifndef MD5_DIGEST_LENGTH
#define MD5_DIGEST_LENGTH 16
#endif

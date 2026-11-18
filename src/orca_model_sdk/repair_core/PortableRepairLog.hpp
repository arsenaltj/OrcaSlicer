#pragma once
// SDK-only diagnostic sink. Geometry code and its decisions are unchanged;
// do not import the desktop application's global Boost.Log owner into the SDK.
#include <boost/log/trivial.hpp>
#include <iostream>
#undef BOOST_LOG_TRIVIAL
#define BOOST_LOG_TRIVIAL(level) std::clog

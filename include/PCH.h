#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMMNOSOUND
#define NOMINMAX  // or windows.h's min/max macros eat every std::min / std::max

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

// wingdi.h defines GetObject as GetObjectW, which silently renames BSScript::Variable::GetObject at every call site.
#ifdef GetObject
#	undef GetObject
#endif

#include <spdlog/sinks/basic_file_sink.h>

namespace logger = SKSE::log;

using namespace std::literals;

namespace stl
{
	using namespace SKSE::stl;
}

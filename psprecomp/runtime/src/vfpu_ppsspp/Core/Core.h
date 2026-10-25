// Minimal stand-in for PPSSPP's Core/Core.h used only by the VFPU semantics host test.
#pragma once
#include <string_view>
#include "Common/CommonTypes.h"
enum class MemoryExceptionType { NONE, READ_WORD, WRITE_WORD, READ_BLOCK, WRITE_BLOCK, ALIGNMENT };
inline void Core_MemoryException(u32, u32, u32, MemoryExceptionType, std::string_view = "") {}

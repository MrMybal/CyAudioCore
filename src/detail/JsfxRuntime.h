#pragma once
#include "ysfx.h"
namespace cyaudio::detail {
bool jsfxMemoryWithinBudget(ysfx_t*) noexcept;
bool jsfxPrepareMemory(ysfx_t*) noexcept;
} // namespace cyaudio::detail

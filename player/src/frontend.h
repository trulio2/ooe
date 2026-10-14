#pragma once

// Calls from the melonDS platform layer back into the app. `userdata` is the
// pointer the app passed to the NDS and cart constructors.

#include <cstdint>

namespace frontend {
void WriteNDSSave(void* userdata, const uint8_t* data, uint32_t length);
void SignalStop(void* userdata, int reason);
}  // namespace frontend

#pragma once

#include <cstdint>
#include <string_view>

namespace tsl {

// DOM KeyboardEvent.code ("KeyA", "ArrowUp", ...) to the USB HID keyboard usage that
// Steam expects as a scancode. Returns 0 for codes without a mapping.
uint32_t ScancodeFromDomCode(std::string_view code);

}  // namespace tsl

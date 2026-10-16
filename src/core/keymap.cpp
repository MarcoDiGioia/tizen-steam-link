#include "core/keymap.h"

#include <cstddef>

namespace tsl {
namespace {

struct Entry {
    const char *code;
    uint32_t usage;
};

// USB HID Usage Tables, Keyboard/Keypad page (0x07).
constexpr Entry kTable[] = {
        {"Enter", 40}, {"Escape", 41}, {"Backspace", 42}, {"Tab", 43}, {"Space", 44},
        {"Minus", 45}, {"Equal", 46}, {"BracketLeft", 47}, {"BracketRight", 48}, {"Backslash", 49},
        {"Semicolon", 51}, {"Quote", 52}, {"Backquote", 53}, {"Comma", 54}, {"Period", 55},
        {"Slash", 56}, {"CapsLock", 57},
        {"F1", 58}, {"F2", 59}, {"F3", 60}, {"F4", 61}, {"F5", 62}, {"F6", 63},
        {"F7", 64}, {"F8", 65}, {"F9", 66}, {"F10", 67}, {"F11", 68}, {"F12", 69},
        {"PrintScreen", 70}, {"ScrollLock", 71}, {"Pause", 72}, {"Insert", 73}, {"Home", 74},
        {"PageUp", 75}, {"Delete", 76}, {"End", 77}, {"PageDown", 78},
        {"ArrowRight", 79}, {"ArrowLeft", 80}, {"ArrowDown", 81}, {"ArrowUp", 82},
        {"NumLock", 83}, {"NumpadDivide", 84}, {"NumpadMultiply", 85}, {"NumpadSubtract", 86},
        {"NumpadAdd", 87}, {"NumpadEnter", 88},
        {"Numpad1", 89}, {"Numpad2", 90}, {"Numpad3", 91}, {"Numpad4", 92}, {"Numpad5", 93},
        {"Numpad6", 94}, {"Numpad7", 95}, {"Numpad8", 96}, {"Numpad9", 97}, {"Numpad0", 98},
        {"NumpadDecimal", 99}, {"IntlBackslash", 100}, {"ContextMenu", 101}, {"NumpadEqual", 103},
        {"F13", 104}, {"F14", 105}, {"F15", 106}, {"F16", 107}, {"F17", 108}, {"F18", 109},
        {"F19", 110}, {"F20", 111}, {"F21", 112}, {"F22", 113}, {"F23", 114}, {"F24", 115},
        {"AudioVolumeMute", 127}, {"AudioVolumeUp", 128}, {"AudioVolumeDown", 129},
        {"NumpadComma", 133}, {"IntlRo", 135}, {"KanaMode", 136}, {"IntlYen", 137},
        {"Convert", 138}, {"NonConvert", 139}, {"Lang1", 144}, {"Lang2", 145},
        {"ControlLeft", 224}, {"ShiftLeft", 225}, {"AltLeft", 226}, {"MetaLeft", 227},
        {"ControlRight", 228}, {"ShiftRight", 229}, {"AltRight", 230}, {"MetaRight", 231},
};

}  // namespace

uint32_t ScancodeFromDomCode(std::string_view code) {
    // "KeyA".."KeyZ" -> 4..29
    if (code.size() == 4 && code.substr(0, 3) == "Key" && code[3] >= 'A' && code[3] <= 'Z') {
        return 4 + static_cast<uint32_t>(code[3] - 'A');
    }
    // "Digit1".."Digit9" -> 30..38, "Digit0" -> 39
    if (code.size() == 6 && code.substr(0, 5) == "Digit" && code[5] >= '0' && code[5] <= '9') {
        return code[5] == '0' ? 39 : 30 + static_cast<uint32_t>(code[5] - '1');
    }
    for (const Entry &e : kTable) {
        if (code == e.code) return e.usage;
    }
    return 0;
}

}  // namespace tsl

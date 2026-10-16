#include "core/identity.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tsl {
namespace {

constexpr std::string_view kPrefix = "tsl1:";

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string HexEncode(const uint8_t *data, size_t len) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0F]);
    }
    return out;
}

bool HexDecode(std::string_view hex, uint8_t *out, size_t len) {
    if (hex.size() != len * 2) return false;
    for (size_t i = 0; i < len; i++) {
        int hi = HexValue(hex[i * 2]);
        int lo = HexValue(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

Identity Identity::FromRandom(const uint8_t (&bytes)[kRandomBytes], std::string name) {
    Identity id;
    for (int i = 0; i < 8; i++) {
        id.deviceId = (id.deviceId << 8) | bytes[i];
    }
    std::memcpy(id.secret.data(), bytes + 8, id.secret.size());
    id.name = std::move(name);
    return id;
}

std::string Identity::Serialize() const {
    uint8_t idBytes[8];
    for (int i = 0; i < 8; i++) {
        idBytes[i] = static_cast<uint8_t>(deviceId >> (56 - i * 8));
    }
    std::string out(kPrefix);
    out += HexEncode(idBytes, sizeof(idBytes));
    out += ':';
    out += HexEncode(secret.data(), secret.size());
    out += ':';
    out += name;
    return out;
}

std::optional<Identity> Identity::Parse(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    if (text.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    text.remove_prefix(kPrefix.size());
    // 16 hex, ':', 64 hex, ':', name (may itself contain ':')
    if (text.size() < 16 + 1 + 64 + 1 || text[16] != ':' || text[16 + 1 + 64] != ':') return std::nullopt;
    uint8_t idBytes[8];
    Identity id;
    if (!HexDecode(text.substr(0, 16), idBytes, sizeof(idBytes))) return std::nullopt;
    if (!HexDecode(text.substr(17, 64), id.secret.data(), id.secret.size())) return std::nullopt;
    for (uint8_t b : idBytes) {
        id.deviceId = (id.deviceId << 8) | b;
    }
    id.name = std::string(text.substr(16 + 1 + 64 + 1));
    if (id.name.empty()) return std::nullopt;
    return id;
}

IHS_ClientConfig Identity::ClientConfig() const {
    IHS_ClientConfig config;
    config.deviceId = deviceId;
    config.secretKey = secret.data();
    config.deviceName = name.c_str();
    return config;
}

std::string FormatPin(uint32_t random) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04u", random % 10000u);
    return buf;
}

std::string IpToString(const IHS_IPAddress &address) {
    char *str = IHS_IPAddressToString(&address);
    std::string out = str != nullptr ? str : "";
    std::free(str);
    return out;
}

}  // namespace tsl

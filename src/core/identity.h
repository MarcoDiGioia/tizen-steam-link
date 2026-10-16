#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/ihs.h"

namespace tsl {

// What Steam knows this device by. Steam pairs (device id, secret) once with a PIN and
// accepts streaming requests from it afterwards, so it has to persist across runs.
struct Identity {
    static constexpr size_t kRandomBytes = 8 + 32;

    uint64_t deviceId = 0;
    std::array<uint8_t, 32> secret{};
    std::string name;

    // bytes must come from a CSPRNG: 8 for the device id, 32 for the secret.
    static Identity FromRandom(const uint8_t (&bytes)[kRandomBytes], std::string name);

    // "tsl1:<16 hex>:<64 hex>:<name>"
    std::string Serialize() const;
    static std::optional<Identity> Parse(std::string_view text);

    // Valid while this Identity is alive and unmoved.
    IHS_ClientConfig ClientConfig() const;
};

std::string HexEncode(const uint8_t *data, size_t len);
bool HexDecode(std::string_view hex, uint8_t *out, size_t len);

// Four digits, zero padded, the format Steam's pairing dialog asks for.
std::string FormatPin(uint32_t random);

std::string IpToString(const IHS_IPAddress &address);

}  // namespace tsl

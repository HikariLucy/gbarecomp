// sha256.h — minimal SHA-256 (FIPS 180-4) for verifying executable images.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gbarecomp {

struct Sha256Digest {
    std::array<uint8_t, 32> bytes{};
    std::string hex() const;
};

Sha256Digest sha256(const void* data, std::size_t len);

}  // namespace gbarecomp

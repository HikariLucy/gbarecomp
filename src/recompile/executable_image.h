// executable_image.h — Immutable executable image contract and registry.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gbarecomp {

struct ExecutableImage {
    std::string id;
    uint32_t    load_address = 0;
    std::size_t size = 0;
    const uint8_t* data = nullptr;
    std::shared_ptr<std::vector<uint8_t>> storage;
    std::string path;
    std::string sha256;
    std::string note;
};

class ExecutableImageRegistry {
public:
    ExecutableImageRegistry() = default;

    // Register an executable image. Validates non-empty id, non-zero size,
    // data pointer, absence of address-space overlaps with previously
    // registered images, and optional SHA-256 match.
    bool add_image(const ExecutableImage& img, std::string* error = nullptr);

    // Look up the image containing guest_pc. If found, returns pointer to image
    // and sets *out_offset = guest_pc - img.load_address. Otherwise returns nullptr.
    const ExecutableImage* find_image_for(uint32_t guest_pc,
                                          std::size_t* out_offset = nullptr) const;

    // Returns true if [addr, addr + len) falls entirely within a single image.
    bool can_read(uint32_t addr, std::size_t len) const;

    const std::vector<ExecutableImage>& images() const { return images_; }
    void clear() { images_.clear(); }

private:
    std::vector<ExecutableImage> images_;
};

}  // namespace gbarecomp

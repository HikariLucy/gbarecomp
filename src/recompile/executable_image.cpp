// executable_image.cpp — Immutable executable image contract and registry.

#include "executable_image.h"
#include "sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace gbarecomp {

namespace {

std::string to_lower_hex(const std::string& s) {
    std::string out = s;
    if (out.size() > 2 && out[0] == '0' && (out[1] == 'x' || out[1] == 'X')) {
        out.erase(0, 2);
    }
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

bool ExecutableImageRegistry::add_image(const ExecutableImage& img,
                                        std::string* error) {
    if (img.id.empty()) {
        if (error) *error = "executable image has empty id";
        return false;
    }
    if (img.size == 0) {
        if (error) *error = "executable image '" + img.id + "' has size 0";
        return false;
    }
    if (img.data == nullptr) {
        if (error) *error = "executable image '" + img.id + "' has null data pointer";
        return false;
    }

    // Check id uniqueness
    for (const auto& ex : images_) {
        if (ex.id == img.id) {
            if (error) *error = "duplicate executable image id '" + img.id + "'";
            return false;
        }
    }

    // Check address overlap
    const uint64_t new_start = img.load_address;
    const uint64_t new_end = new_start + img.size;
    for (const auto& ex : images_) {
        const uint64_t ex_start = ex.load_address;
        const uint64_t ex_end = ex_start + ex.size;
        if (std::max(new_start, ex_start) < std::min(new_end, ex_end)) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "executable image range overlap: '%s' [0x%08X, 0x%08X) "
                          "conflicts with '%s' [0x%08X, 0x%08X)",
                          ex.id.c_str(), static_cast<uint32_t>(ex_start),
                          static_cast<uint32_t>(ex_end),
                          img.id.c_str(), static_cast<uint32_t>(new_start),
                          static_cast<uint32_t>(new_end));
            if (error) *error = buf;
            return false;
        }
    }

    // SHA-256 verification
    if (!img.sha256.empty()) {
        const std::string expected = to_lower_hex(img.sha256);
        const std::string actual = sha256(img.data, img.size).hex();
        if (expected != actual) {
            if (error) {
                *error = "executable image '" + img.id + "' sha256 mismatch: expected " +
                         expected + ", computed " + actual;
            }
            return false;
        }
    }

    images_.push_back(img);
    return true;
}

const ExecutableImage* ExecutableImageRegistry::find_image_for(
    uint32_t guest_pc, std::size_t* out_offset) const {
    for (const auto& img : images_) {
        if (guest_pc >= img.load_address &&
            (guest_pc - img.load_address) < img.size) {
            if (out_offset) {
                *out_offset = static_cast<std::size_t>(guest_pc - img.load_address);
            }
            return &img;
        }
    }
    return nullptr;
}

bool ExecutableImageRegistry::can_read(uint32_t addr, std::size_t len) const {
    std::size_t off = 0;
    const auto* img = find_image_for(addr, &off);
    if (!img) return false;
    return (off + len) <= img->size;
}

}  // namespace gbarecomp

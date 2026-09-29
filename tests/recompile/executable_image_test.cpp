#include "executable_image.h"
#include "sha256.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int fail(const char* msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

}  // namespace

int main() {
    gbarecomp::ExecutableImageRegistry reg;
    std::string err;

    std::vector<uint8_t> data1(0x100, 0x55);
    std::vector<uint8_t> data2(0x80, 0xAA);

    // 1. Empty ID rejected
    gbarecomp::ExecutableImage empty_id;
    empty_id.id = "";
    empty_id.load_address = 0x03001000;
    empty_id.size = data1.size();
    empty_id.data = data1.data();
    if (reg.add_image(empty_id, &err)) {
        return fail("empty ID was accepted");
    }

    // 2. Zero size rejected
    gbarecomp::ExecutableImage zero_sz;
    zero_sz.id = "img0";
    zero_sz.load_address = 0x03001000;
    zero_sz.size = 0;
    zero_sz.data = data1.data();
    if (reg.add_image(zero_sz, &err)) {
        return fail("zero size was accepted");
    }

    // 3. Null data rejected
    gbarecomp::ExecutableImage null_data;
    null_data.id = "img0";
    null_data.load_address = 0x03001000;
    null_data.size = data1.size();
    null_data.data = nullptr;
    if (reg.add_image(null_data, &err)) {
        return fail("null data was accepted");
    }

    // 4. Valid image 1
    gbarecomp::ExecutableImage img1;
    img1.id = "img1";
    img1.load_address = 0x03001000;
    img1.size = data1.size();
    img1.data = data1.data();
    img1.sha256 = gbarecomp::sha256(data1.data(), data1.size()).hex();
    if (!reg.add_image(img1, &err)) {
        return fail(("failed to add valid img1: " + err).c_str());
    }

    // 5. Duplicate ID rejected
    gbarecomp::ExecutableImage dup_id;
    dup_id.id = "img1";
    dup_id.load_address = 0x03002000;
    dup_id.size = data2.size();
    dup_id.data = data2.data();
    if (reg.add_image(dup_id, &err)) {
        return fail("duplicate ID was accepted");
    }

    // 6. Overlapping address space rejected
    gbarecomp::ExecutableImage overlap;
    overlap.id = "overlap";
    overlap.load_address = 0x03001080;  // img1 is [0x03001000, 0x03001100)
    overlap.size = data2.size();
    overlap.data = data2.data();
    if (reg.add_image(overlap, &err)) {
        return fail("overlapping range was accepted");
    }

    // 7. Bad SHA-256 rejected
    gbarecomp::ExecutableImage bad_hash;
    bad_hash.id = "bad_hash";
    bad_hash.load_address = 0x03002000;
    bad_hash.size = data2.size();
    bad_hash.data = data2.data();
    bad_hash.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    if (reg.add_image(bad_hash, &err)) {
        return fail("mismatched SHA-256 was accepted");
    }

    // 8. Valid image 2
    gbarecomp::ExecutableImage img2;
    img2.id = "img2";
    img2.load_address = 0x03002000;
    img2.size = data2.size();
    img2.data = data2.data();
    img2.sha256 = gbarecomp::sha256(data2.data(), data2.size()).hex();
    if (!reg.add_image(img2, &err)) {
        return fail(("failed to add valid img2: " + err).c_str());
    }

    // 9. Lookup tests
    std::size_t off = 0;
    const auto* found = reg.find_image_for(0x03001050, &off);
    if (!found || found->id != "img1" || off != 0x50) {
        return fail("lookup in img1 failed or returned wrong offset");
    }

    found = reg.find_image_for(0x03002020, &off);
    if (!found || found->id != "img2" || off != 0x20) {
        return fail("lookup in img2 failed or returned wrong offset");
    }

    found = reg.find_image_for(0x03001500, &off);
    if (found != nullptr) {
        return fail("unmapped address returned an image");
    }

    // 10. can_read tests
    if (!reg.can_read(0x03001000, 0x100)) {
        return fail("can_read whole img1 returned false");
    }
    if (reg.can_read(0x030010F0, 0x20)) {
        return fail("can_read crossing img1 boundary returned true");
    }
    if (reg.can_read(0x03001500, 4)) {
        return fail("can_read on unmapped address returned true");
    }

    std::printf("executable_image_tests: PASS\n");
    return 0;
}

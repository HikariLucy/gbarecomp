// Private-lookup index semantics against a linear-scan reference (the
// pre-index runtime behaviour): hit, miss, wrong image, wrong mode, duplicate
// keys (first table row wins), and independence from nothing but table order.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>

#include "dispatch_table_entries.h"

using gbarecomp::DispatchTableEntrySpec;

static void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

// What the generated runtime does with the index.
static long indexed(const std::vector<DispatchTableEntrySpec>& t,
                    const std::vector<gbarecomp::PrivateIndexRow>& ix,
                    unsigned image, std::uint32_t pc, int thumb) {
    const std::uint64_t key = gbarecomp::private_lookup_key(
        static_cast<std::uint16_t>(image), thumb != 0, pc & ~1u);
    auto it = std::lower_bound(ix.begin(), ix.end(), key,
        [](const gbarecomp::PrivateIndexRow& r, std::uint64_t k) { return r.key < k; });
    if (it == ix.end() || it->key != key) return -1;
    (void)t;
    return static_cast<long>(it->slot);
}
// The original generated loop.
static long linear(const std::vector<DispatchTableEntrySpec>& t,
                   unsigned image, std::uint32_t pc, int thumb) {
    for (std::size_t i = 0; i < t.size(); ++i)
        if (t[i].domain == image && t[i].addr == (pc & ~1u) && t[i].thumb == (thumb != 0))
            return static_cast<long>(i);
    return -1;
}

int main() {
    std::vector<DispatchTableEntrySpec> t;
    // image, addr, thumb, resume
    auto add = [&](unsigned image, std::uint32_t addr, bool thumb, bool resume, std::size_t fn) {
        t.push_back({addr, thumb, resume, fn, static_cast<std::uint16_t>(image)});
    };
    add(0, 0x03001000, true, false, 0);
    add(0, 0x03001002, true, true, 0);
    add(1, 0x03001000, true, false, 1);   // same PC, other image
    add(1, 0x03001000, false, false, 2);  // same PC+image, other mode
    add(2, 0x06006558, false, false, 3);
    add(2, 0x06006558, false, true, 4);   // duplicate key: root first, alias second
    add(2, 0x06006558, false, true, 5);   // and a third
    add(7, 0x0203E000, true, false, 6);
    add(0, 0x03001000, true, true, 7);    // duplicate of row 0, listed late
    const auto ix = gbarecomp::build_private_lookup_index(t);
    check(ix.size() == t.size(), "index size");
    for (std::size_t i = 1; i < ix.size(); ++i)
        check(ix[i - 1].key < ix[i].key || (ix[i - 1].key == ix[i].key && ix[i - 1].slot < ix[i].slot),
              "index order (key, then table order)");
    // explicit expectations
    check(indexed(t, ix, 0, 0x03001000, 1) == 0, "hit");
    check(indexed(t, ix, 0, 0x03001001, 1) == 0, "pc&~1 normalisation");
    check(indexed(t, ix, 1, 0x03001000, 1) == 2, "same pc, image 1");
    check(indexed(t, ix, 1, 0x03001000, 0) == 3, "same pc/image, ARM");
    check(indexed(t, ix, 3, 0x03001000, 1) == -1, "wrong image");
    check(indexed(t, ix, 0, 0x03001000, 0) == -1, "wrong mode");
    check(indexed(t, ix, 0, 0x03001004, 1) == -1, "miss");
    check(indexed(t, ix, 2, 0x06006558, 0) == 4, "duplicate key: first table row wins");
    check(indexed(t, ix, 0, 0x03001000, 1) == 0, "duplicate key listed late loses");
    // exhaustive agreement with the linear reference over a PC/image/mode grid
    std::uint32_t pcs[] = {0x03000FFE, 0x03001000, 0x03001001, 0x03001002, 0x03001004,
                           0x06006558, 0x06006559, 0x0203E000, 0x0203E002, 0x08000000, 0xFFFFFFFE};
    for (unsigned image = 0; image < 9; ++image)
        for (std::uint32_t pc : pcs)
            for (int thumb = 0; thumb < 2; ++thumb)
                check(indexed(t, ix, image, pc, thumb) == linear(t, image, pc, thumb),
                      "index disagrees with linear first-match");
    // empty table
    check(gbarecomp::build_private_lookup_index({}).empty(), "empty index");
    std::puts("private_lookup_index_tests: ok");
    return 0;
}

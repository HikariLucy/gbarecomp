#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gbarecomp {

struct Function;

struct DispatchTableEntrySpec {
    std::uint32_t addr;
    bool thumb;
    bool resume;
    std::size_t function_index;
    // Image identity of the owning function (0 = public/primary world).
    std::uint16_t domain = 0;
};

// Flatten function roots and resume aliases into the global ordering required
// by runtime_arm's binary search. ARM and Thumb bodies may overlap at the same
// numeric addresses, so sorting each host followed by its aliases is not
// sufficient when an alias crosses the next host.
std::vector<DispatchTableEntrySpec> build_dispatch_table_entries(
    const std::vector<Function>& functions);
std::vector<DispatchTableEntrySpec> build_private_dispatch_table_entries(
    const std::vector<Function>& functions);

// Private-entry lookup index. The generated runtime binary-searches rows sorted
// by key (image<<33 | thumb<<32 | pc); equal keys keep table order so the
// search reproduces the first-match behaviour of a linear scan of the table.
struct PrivateIndexRow {
    std::uint64_t key;
    std::size_t slot;  // row of the private dispatch table
};
std::uint64_t private_lookup_key(std::uint16_t image, bool thumb, std::uint32_t pc);
std::vector<PrivateIndexRow> build_private_lookup_index(
    const std::vector<DispatchTableEntrySpec>& private_entries);

}  // namespace gbarecomp

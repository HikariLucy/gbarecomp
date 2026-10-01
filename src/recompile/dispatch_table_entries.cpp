#include "dispatch_table_entries.h"

#include <algorithm>

#include "function_finder.h"

namespace gbarecomp {

static std::vector<DispatchTableEntrySpec> build_entries(
    const std::vector<Function>& functions, bool private_only) {
    std::vector<DispatchTableEntrySpec> entries;
    std::size_t count = functions.size();
    for (const Function& function : functions) {
        count += function.alias_entries.size();
    }
    entries.reserve(count);

    for (std::size_t i = 0; i < functions.size(); ++i) {
        const Function& function = functions[i];
        if (function.private_entry != private_only) continue;
        const bool thumb = function.mode == CpuMode::Thumb;
        entries.push_back({function.addr, thumb, false, i, function.domain});
        for (std::uint32_t addr : function.alias_entries) {
            entries.push_back({addr, thumb, true, i, function.domain});
        }
    }

    std::stable_sort(
        entries.begin(), entries.end(),
        [](const DispatchTableEntrySpec& lhs,
           const DispatchTableEntrySpec& rhs) {
            if (lhs.domain != rhs.domain) return lhs.domain < rhs.domain;
            if (lhs.addr != rhs.addr) return lhs.addr < rhs.addr;
            if (lhs.thumb != rhs.thumb) return lhs.thumb < rhs.thumb;
            // Prefer a real function root when overlapping discovery also
            // produced a same-mode resume alias at the identical address.
            if (lhs.resume != rhs.resume) return lhs.resume < rhs.resume;
            return lhs.function_index < rhs.function_index;
        });
    return entries;
}

std::vector<DispatchTableEntrySpec> build_dispatch_table_entries(
    const std::vector<Function>& functions) {
    return build_entries(functions, false);
}

std::vector<DispatchTableEntrySpec> build_private_dispatch_table_entries(
    const std::vector<Function>& functions) {
    return build_entries(functions, true);
}

std::uint64_t private_lookup_key(std::uint16_t image, bool thumb,
                                 std::uint32_t pc) {
    return (static_cast<std::uint64_t>(image) << 33) |
           (static_cast<std::uint64_t>(thumb ? 1u : 0u) << 32) | pc;
}

std::vector<PrivateIndexRow> build_private_lookup_index(
    const std::vector<DispatchTableEntrySpec>& private_entries) {
    std::vector<PrivateIndexRow> index;
    index.reserve(private_entries.size());
    for (std::size_t i = 0; i < private_entries.size(); ++i) {
        const auto& e = private_entries[i];
        index.push_back({private_lookup_key(e.domain, e.thumb, e.addr), i});
    }
    std::sort(index.begin(), index.end(),
              [](const PrivateIndexRow& a, const PrivateIndexRow& b) {
                  return a.key != b.key ? a.key < b.key : a.slot < b.slot;
              });
    return index;
}

}  // namespace gbarecomp

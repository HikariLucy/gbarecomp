#include "function_finder.h"
#include "dispatch_table_entries.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::uint32_t kBase = 0x08000000u;

void put16(std::vector<std::uint8_t>& rom, std::size_t off,
           std::uint16_t value) {
    rom[off] = static_cast<std::uint8_t>(value);
    rom[off + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& rom, std::size_t off,
           std::uint32_t value) {
    put16(rom, off, static_cast<std::uint16_t>(value));
    put16(rom, off + 2, static_cast<std::uint16_t>(value >> 16));
}

bool has_function(const gbarecomp::FunctionFinder& finder,
                  std::uint32_t addr, gbarecomp::CpuMode mode) {
    for (const auto& fn : finder.functions()) {
        if (fn.addr == addr && fn.mode == mode) return true;
    }
    return false;
}

const gbarecomp::Function* find_function(
    const gbarecomp::FunctionFinder& finder,
    std::uint32_t addr,
    gbarecomp::CpuMode mode) {
    for (const auto& fn : finder.functions()) {
        if (fn.addr == addr && fn.mode == mode) return &fn;
    }
    return nullptr;
}

}  // namespace

int main() {
    // SoftReset closes both instruction sets; ordinary BIOS calls resume.
    for (const bool thumb : {false, true}) {
        std::vector<std::uint8_t> image(0x80, 0);
        const auto mode = thumb ? gbarecomp::CpuMode::Thumb : gbarecomp::CpuMode::Arm;
        const std::uint32_t step = thumb ? 2u : 4u;
        for (const std::uint32_t swi_num : {0u, 1u, 0x11u}) {
            if (thumb) put16(image, 0, static_cast<std::uint16_t>(0xDF00u | swi_num));
            else put32(image, 0, 0xEF000000u | (swi_num << 16));
            gbarecomp::FunctionFinder finder(image.data(), image.size(), kBase);
            finder.set_speculative_literal_harvest(false);
            finder.add_seed({kBase, mode, "swi_fixture"});
            finder.add_data_range(kBase + step, kBase + step * 2u, "literal");
            finder.run(8);
            const auto* fn = find_function(finder, kBase, mode);
            if (!fn || (swi_num == 0u ? !finder.collisions().empty() :
                                           finder.collisions().empty()) ||
                (swi_num == 0u && fn->end_addr != kBase + step)) {
                std::fprintf(stderr, "SWI %u terminal policy wrong (%s)\n",
                             swi_num, thumb ? "Thumb" : "ARM");
                return 1;
            }
        }
    }
    // A non-returning ARM call must end the caller before the declared data.
    {
        std::vector<std::uint8_t> image(0x300, 0);
        put32(image, 0x100, 0xEB00003Eu); // BL 0x08000200
        put32(image, 0x200, 0xE321F013u); // MSR CPSR_c, #0x13 (LR analysis unknown)
        put32(image, 0x204, 0xE12FFF10u); // BX r0
        gbarecomp::FunctionFinder baseline(image.data(), image.size(), kBase);
        baseline.set_speculative_literal_harvest(false);
        baseline.add_seed({kBase + 0x100, gbarecomp::CpuMode::Arm, "caller"});
        baseline.add_data_range(kBase + 0x104, kBase + 0x200, "literal bytes");
        baseline.run(16);
        if (baseline.collisions().empty()) {
            std::fprintf(stderr, "ordinary BL failed to reproduce data collision\n");
            return 1;
        }
        gbarecomp::FunctionFinder explicit_flow(image.data(), image.size(), kBase);
        explicit_flow.set_speculative_literal_harvest(false);
        explicit_flow.add_seed({kBase + 0x100, gbarecomp::CpuMode::Arm, "caller"});
        gbarecomp::FunctionSeed callee{kBase + 0x200, gbarecomp::CpuMode::Arm, "callee"};
        callee.returns = false;
        callee.returns_declared = true;
        explicit_flow.add_seed(callee);
        explicit_flow.add_data_range(kBase + 0x104, kBase + 0x200, "literal bytes");
        explicit_flow.run(16);
        const auto* caller = find_function(explicit_flow, kBase + 0x100,
                                           gbarecomp::CpuMode::Arm);
        if (!explicit_flow.collisions().empty() || !caller ||
            caller->end_addr != kBase + 0x104 ||
            !has_function(explicit_flow, kBase + 0x200, gbarecomp::CpuMode::Arm)) {
            std::fprintf(stderr, "non-returning BL crossed into data\n");
            return 1;
        }
        gbarecomp::FunctionFinder conflicting(image.data(), image.size(), kBase);
        conflicting.add_seed(callee);
        callee.returns = true;
        conflicting.add_seed(callee);
        bool rejected = false;
        try { conflicting.run(8); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected) {
            std::fprintf(stderr, "contradictory returns declarations accepted\n");
            return 1;
        }
    }
    std::vector<std::uint8_t> rom(0x80, 0);

    // ldr r1,[pc,#8]  -> tracker sees the even ROM data pointer below
    // pop {r1}        -> r1 is overwritten from the runtime stack
    // bx r1           -> dynamic return; must not reuse the stale literal
    put16(rom, 0x00, 0x4902u);
    put16(rom, 0x02, 0xBC02u);
    put16(rom, 0x04, 0x4708u);
    put32(rom, 0x0C, kBase + 0x20u);

    // Make the false target valid-looking ARM code. Before the regression
    // fix, the stale constant caused this address to be emitted.
    put32(rom, 0x20, 0xE12FFF1Eu);  // bx lr

    gbarecomp::FunctionFinder finder(rom.data(), rom.size(), kBase);
    finder.set_speculative_literal_harvest(false);
    finder.add_seed({kBase, gbarecomp::CpuMode::Thumb, "pop_bx_return"});
    finder.run(32);

    if (!has_function(finder, kBase, gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr, "entry function was not discovered\n");
        return 1;
    }
    if (has_function(finder, kBase + 0x20u, gbarecomp::CpuMode::Arm)) {
        std::fprintf(stderr,
                     "POP destination retained a stale constant and seeded "
                     "0x%08X as ARM code\n",
                     kBase + 0x20u);
        return 1;
    }

    // A stack-local helper may be copied again at an overlapping address.
    // The explicit source address must win over the first fixed mapping, and
    // CFG roots from the older placement must not truncate the newer body.
    std::vector<std::uint8_t> relocated_rom(0x80, 0);
    constexpr std::size_t kTemplate = 0x40;
    put16(relocated_rom, kTemplate + 0x00, 0x2800u); // cmp r0,#0
    put16(relocated_rom, kTemplate + 0x02, 0xD001u); // beq +8
    put16(relocated_rom, kTemplate + 0x04, 0x4770u); // bx lr
    put16(relocated_rom, kTemplate + 0x06, 0x46C0u); // nop
    put16(relocated_rom, kTemplate + 0x08, 0x4770u); // bx lr

    constexpr std::uint32_t kRam = 0x03000000u;
    gbarecomp::FunctionFinder relocated(
        relocated_rom.data(), relocated_rom.size(), kBase);
    relocated.set_speculative_literal_harvest(false);
    relocated.add_code_copy(kRam, kBase + kTemplate, 0x20, "first copy");
    relocated.add_seed({kRam, gbarecomp::CpuMode::Thumb, "first"});
    gbarecomp::FunctionSeed older_public{
        kRam + 4, gbarecomp::CpuMode::Thumb, "older_public_seed"};
    older_public.source_addr = kBase + kTemplate + 4;
    relocated.add_seed(older_public);
    gbarecomp::FunctionSeed overlapping{
        kRam + 4, gbarecomp::CpuMode::Thumb, "overlapping"};
    overlapping.source_addr = kBase + kTemplate;
    overlapping.private_entry = true;
    overlapping.dispatch_declared = true;
    relocated.add_seed(overlapping);
    relocated.add_seed({kRam + 12, gbarecomp::CpuMode::Thumb, "older_target"});
    relocated.run(32);

    const auto* overlap_fn = find_function(
        relocated, kRam + 4, gbarecomp::CpuMode::Thumb);
    if (!overlap_fn || !overlap_fn->private_entry ||
        overlap_fn->source_addr != kBase + kTemplate) {
        std::fprintf(stderr,
                     "overlapping relocation did not retain explicit ROM backing\n");
        return 1;
    }
    if (std::find(overlap_fn->alias_entries.begin(),
                  overlap_fn->alias_entries.end(), kRam + 6u) ==
        overlap_fn->alias_entries.end()) {
        std::fprintf(stderr, "private function lacks interior resume alias\n");
        return 1;
    }
    if (overlap_fn->end_addr <= kRam + 8) {
        std::fprintf(stderr,
                     "older placement incorrectly truncated overlapping CFG\n");
        return 1;
    }
    const auto* overlap_target = find_function(
        relocated, kRam + 12, gbarecomp::CpuMode::Thumb);
    if (!overlap_target || !overlap_target->private_entry ||
        overlap_target->source_addr != kBase + kTemplate + 8) {
        std::fprintf(stderr,
                     "relocation bias did not propagate to direct branch target\n");
        return 1;
    }
    for (const auto& entry :
         gbarecomp::build_dispatch_table_entries(relocated.functions())) {
        if (entry.addr == kRam + 4u || entry.addr == kRam + 12u) {
            std::fprintf(stderr, "auto discovery published private RAM PC\n");
            return 1;
        }
    }

    // An explicit public declaration at the same placement is ambiguous.
    gbarecomp::FunctionFinder conflicting(
        relocated_rom.data(), relocated_rom.size(), kBase);
    conflicting.set_speculative_literal_harvest(false);
    auto public_claim = overlapping;
    public_claim.private_entry = false;
    conflicting.add_seed(public_claim);
    conflicting.add_seed(overlapping);
    bool rejected = false;
    try { conflicting.run(32); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) {
        std::fprintf(stderr, "conflicting dispatch declarations accepted\n");
        return 1;
    }

    // A private RAM body can BX through a literal to a public ROM body.
    std::vector<std::uint8_t> external_rom(0x80, 0);
    put16(external_rom, 0x20, 0x4801u); // ldr r0,[pc,#4]
    put16(external_rom, 0x22, 0x4700u); // bx r0
    put32(external_rom, 0x28, kBase + 0x61u);
    put16(external_rom, 0x60, 0x4770u); // bx lr
    gbarecomp::FunctionFinder external(
        external_rom.data(), external_rom.size(), kBase);
    external.set_speculative_literal_harvest(false);
    gbarecomp::FunctionSeed private_call{
        kRam, gbarecomp::CpuMode::Thumb, "private_call"};
    private_call.source_addr = kBase + 0x20u;
    private_call.private_entry = true;
    private_call.dispatch_declared = true;
    external.add_seed(private_call);
    external.run(32);
    const auto* rom_callee = find_function(
        external, kBase + 0x60u, gbarecomp::CpuMode::Thumb);
    if (!rom_callee || rom_callee->private_entry) {
        std::fprintf(stderr, "public ROM callee became private\n");
        return 1;
    }

    // A bounded AOT sweep finds compiler-shaped prologues without runtime
    // evidence, then uses two strong entries in an even-address THUMB pointer
    // table to infer the table mode and recover a leaf function with no PUSH.
    std::vector<std::uint8_t> aot_rom(0x100, 0);
    put16(aot_rom, 0x00, 0x4802u);  // ldr r0,[pc,#8] -> table base
    put16(aot_rom, 0x02, 0x4770u);  // bx lr
    put32(aot_rom, 0x0C, kBase + 0xC0u);
    put16(aot_rom, 0x40, 0xB500u);  // push {lr}
    put16(aot_rom, 0x42, 0x46C0u);
    put16(aot_rom, 0x44, 0x46C0u);
    put16(aot_rom, 0x46, 0x46C0u);
    put16(aot_rom, 0x48, 0x46C0u);
    put16(aot_rom, 0x4A, 0xBD00u);  // pop {pc}
    put16(aot_rom, 0x50, 0xB500u);  // second strong table-mode vote
    put16(aot_rom, 0x52, 0x46C0u);
    put16(aot_rom, 0x54, 0x46C0u);
    put16(aot_rom, 0x56, 0x46C0u);
    put16(aot_rom, 0x58, 0x46C0u);
    put16(aot_rom, 0x5A, 0xBD00u);
    put16(aot_rom, 0x60, 0x2001u);  // movs r0,#1 (leaf, no prologue)
    put16(aot_rom, 0x62, 0x46C0u);
    put16(aot_rom, 0x64, 0x46C0u);
    put16(aot_rom, 0x66, 0x46C0u);
    put16(aot_rom, 0x68, 0x46C0u);
    put16(aot_rom, 0x6A, 0x4770u);  // bx lr
    put32(aot_rom, 0xC0, kBase + 0x40u);
    put32(aot_rom, 0xC4, kBase + 0x50u);
    put32(aot_rom, 0xC8, kBase + 0x60u);

    gbarecomp::FunctionFinder aot(aot_rom.data(), aot_rom.size(), kBase);
    aot.set_speculative_literal_harvest(false);
    aot.set_aot_scan_range(kBase + 0x40u, kBase + 0x80u);
    aot.add_seed({kBase, gbarecomp::CpuMode::Thumb, "entry"});
    aot.run(64);
    if (!has_function(aot, kBase + 0x40u, gbarecomp::CpuMode::Thumb) ||
        !has_function(aot, kBase + 0x50u, gbarecomp::CpuMode::Thumb) ||
        !has_function(aot, kBase + 0x60u, gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr,
                     "bounded AOT scan missed a prologue or address-taken "
                     "THUMB leaf\n");
        return 1;
    }
    if (aot.stats().aot_prologue_seeds != 2 ||
        aot.stats().aot_pointer_tables != 1 ||
        aot.stats().aot_pointer_seeds != 1) {
        std::fprintf(stderr,
                     "bounded AOT scan stats disagree: prologues=%zu "
                     "tables=%zu leaves=%zu\n",
                     aot.stats().aot_prologue_seeds,
                     aot.stats().aot_pointer_tables,
                     aot.stats().aot_pointer_seeds);
        return 1;
    }

    // A copied ARM blob can contain internal callable helpers that are not
    // reachable from its declared entry. Scan the ROM backing of a known
    // code-copy range for stack-frame prologues, including leaf helpers that
    // save callee-saved registers without saving LR.
    std::vector<std::uint8_t> copied_rom(0x100, 0);
    constexpr std::size_t kCopiedTemplate = 0x40;
    put32(copied_rom, kCopiedTemplate + 0x00, 0xE12FFF1Eu); // bx lr
    put32(copied_rom, kCopiedTemplate + 0x1C, 0xE1A0C00Du); // mov ip,sp
    put32(copied_rom, kCopiedTemplate + 0x20, 0xE92D0030u); // push {r4,r5}
    put32(copied_rom, kCopiedTemplate + 0x24, 0xE1A00000u); // nop
    put32(copied_rom, kCopiedTemplate + 0x28, 0xE1A00000u);
    put32(copied_rom, kCopiedTemplate + 0x2C, 0xE1A00000u);
    put32(copied_rom, kCopiedTemplate + 0x30, 0xE1A00000u);
    put32(copied_rom, kCopiedTemplate + 0x34, 0xE8BD0030u); // pop {r4,r5}
    put32(copied_rom, kCopiedTemplate + 0x38, 0xE12FFF1Eu); // bx lr

    constexpr std::uint32_t kCopiedRam = 0x03001000u;
    gbarecomp::FunctionFinder copied(
        copied_rom.data(), copied_rom.size(), kBase);
    copied.set_speculative_literal_harvest(false);
    copied.set_static_resume_all(true);
    copied.add_code_copy(
        kCopiedRam, kBase + kCopiedTemplate, 0x40, "ARM helper blob");
    copied.add_seed(
        {kCopiedRam, gbarecomp::CpuMode::Arm, "copied_entry"});
    copied.run(32);
    const auto* copied_helper = find_function(
        copied, kCopiedRam + 0x1Cu, gbarecomp::CpuMode::Arm);
    bool copied_push_resume = false;
    if (copied_helper) {
        for (std::uint32_t pc : copied_helper->alias_entries) {
            if (pc == kCopiedRam + 0x20u) copied_push_resume = true;
        }
    }
    if (!copied_helper ||
        copied_helper->source_addr != kBase + kCopiedTemplate + 0x1Cu ||
        copied.stats().aot_code_copy_seeds != 1 ||
        !copied_push_resume) {
        std::fprintf(stderr,
                     "code-copy scan missed internal ARM helper: fn=%s "
                     "source=0x%08X seeds=%zu push_resume=%s\n",
                     copied_helper ? "yes" : "no",
                     copied_helper ? copied_helper->source_addr : 0u,
                     copied.stats().aot_code_copy_seeds,
                     copied_push_resume ? "yes" : "no");
        return 1;
    }

    // External executable image discovery test:
    // Secondary image at 0x03007400 (ARM), containing:
    //   0x03007400: mov r1, #0x14 (0xE3A01014)
    //   0x03007404: b +8 -> 0x0300740C (0xEA000000)
    //   0x03007408: nop (0xE1A00000)
    //   0x0300740C: bx lr (0xE12FFF1E)
    std::vector<std::uint8_t> primary_rom(0x100, 0);
    put32(primary_rom, 0, 0xE12FFF1Eu); // bx lr

    std::vector<std::uint8_t> ext_image(0x40, 0);
    constexpr std::uint32_t kExtBase = 0x03007400u;
    put32(ext_image, 0x00, 0xE3A01014u); // mov r1, #0x14
    put32(ext_image, 0x04, 0xEA000000u); // b 0x0300740C
    put32(ext_image, 0x08, 0xE1A00000u); // nop
    put32(ext_image, 0x0C, 0xE12FFF1Eu); // bx lr

    gbarecomp::FunctionFinder ext_finder(
        primary_rom.data(), primary_rom.size(), kBase);
    ext_finder.set_speculative_literal_harvest(false);

    gbarecomp::ExecutableImage img;
    img.id = "nes_payload";
    img.load_address = kExtBase;
    img.size = ext_image.size();
    img.data = ext_image.data();
    std::string ext_err;
    if (!ext_finder.add_executable_image(img, &ext_err)) {
        std::fprintf(stderr, "add_executable_image failed: %s\n", ext_err.c_str());
        return 1;
    }

    ext_finder.add_seed({kBase, gbarecomp::CpuMode::Arm, "rom_entry"});
    ext_finder.add_seed({kExtBase, gbarecomp::CpuMode::Arm, "ext_entry"});
    ext_finder.run(16);

    const auto* ext_fn = find_function(
        ext_finder, kExtBase, gbarecomp::CpuMode::Arm);
    const auto* ext_target = find_function(
        ext_finder, kExtBase + 0x0Cu, gbarecomp::CpuMode::Arm);

    if (!ext_fn) {
        std::fprintf(stderr, "failed to discover external entry at 0x%08X\n", kExtBase);
        return 1;
    }
    if (ext_fn->source_addr != kExtBase) {
        std::fprintf(stderr, "external function source_addr expected 0x%08X, got 0x%08X\n",
                     kExtBase, ext_fn->source_addr);
        return 1;
    }
    if (!ext_target) {
        std::fprintf(stderr, "failed to discover internal branch target at 0x%08X in external image\n",
                     kExtBase + 0x0Cu);
        return 1;
    }

    std::printf("function_finder_tests: PASS\n");
    return 0;
}

#include <cstdint>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "gba_bus.h"
#include "gba_io.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "recompiled.h"

extern "C" std::uint32_t g_irq_nest_depth;
extern "C" unsigned long long g_runtime_irq_max_depth;

namespace {
void check(bool value, const char* why) {
    if (!value) { std::fprintf(stderr, "%s\n", why); std::exit(1); }
}
std::uint32_t entry_pc = 0;
std::uint32_t nested_return_pc = 0;
std::uint32_t nested_spsr = 0;
void on_entry(std::uint32_t pc) { if (!entry_pc) entry_pc = pc; }
int verified_ram_hook(std::uint32_t pc, int thumb) {
    if (pc < 0x03001000u || pc >= 0x03001010u || !thumb) return 0;
    auto* bus = gbarecomp::active_bus();
    for (std::uint32_t i = 0; i < 0x30u; ++i)
        if (bus->read8(0x03001000u + i) !=
            bus->read8(0x08000100u + i)) return 0;
    return runtime_invoke_private_entry(pc, thumb);
}
void irq_vector() {
    check(g_irq_nest_depth == 1u || g_irq_nest_depth == 2u,
          "unexpected IRQ depth");
    if (g_irq_nest_depth == 2u) {
        nested_return_pc = g_cpu.R[14] - 4u;
        nested_spsr = runtime_mrs_spsr();
        check(nested_return_pc >= 0x03001000u &&
              nested_return_pc < 0x03001010u, "nested IRQ leaked ROM PC");
        check((nested_spsr & 0x3Fu) == 0x3Fu,
              "nested IRQ did not interrupt System Thumb mode");
        gbarecomp::active_bus()->write16(0x04000202u, 2u);
        runtime_exception_return(nested_return_pc);
        return;
    }
    const std::uint32_t outer_return_pc = g_cpu.R[14] - 4u;
    runtime_msr_cpsr(0x3Fu, 1u); // IntrMain-style IRQ -> System, I clear.
    check(g_irq_nest_depth == 1u && (g_cpu.cpsr & 0x1Fu) == 0x1Fu,
          "IRQ depth did not survive System mode switch");
    auto* bus = gbarecomp::active_bus();
    bus->write16(0x04000200u, 2u); // IE: HBlank
    bus->write16(0x04000208u, 1u); // IME
    bus->io().request_irq(2u);
    g_cpu.R[13] = 0x03007F00u;
    g_cpu.R[14] = 0x08000001u;
    runtime_call_push_return(0x08000000u);
    gf_private_ram_callback();
    check(runtime_call_stack_depth() == 0u,
          "nested callback unbalanced the return stack");
    runtime_msr_cpsr(0x92u, 1u); // return to IRQ mode with I set.
    runtime_exception_return(outer_return_pc);
}
}
struct DispatchEntry {
    std::uint32_t addr;
    std::uint8_t thumb;
    std::uint8_t resume;
    void (*fn)();
};
extern "C" const DispatchEntry kBiosDispatchTable[] = {
    {0x18u, 0u, 0u, irq_vector},
};
extern "C" const unsigned kBiosDispatchTableLen = 1u;

int main(int argc, char** argv) {
    check(argc == 2, "fixture ROM path required");
    setenv("GBARECOMP_MMIO_CAP", "1", 1);
    std::ifstream stream(argv[1], std::ios::binary);
    std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(stream)), {});
    check(rom.size() == 0x200, "synthetic ROM size");
    gba::GbaBus bus;
    gba::GbaPpu ppu;
    bus.set_rom(rom.data(), rom.size());
    gbarecomp::set_active_bus(&bus);
    gbarecomp::set_active_ppu(&ppu);
    for (std::uint32_t i = 0; i < 0x30; ++i)
        bus.write8(0x03001000u + i, rom[0x100 + i]);
    check(!runtime_has_static_entry(0x03001000u, 1), "private root is dispatchable");
    check(!runtime_has_static_entry(0x03001004u, 1), "private resume is dispatchable");
    check(!runtime_has_static_entry(0x0300100Cu, 1), "private CFG child is dispatchable");
    check(runtime_has_static_entry(0x08000100u, 1), "public ROM entry missing");
    // A mismatching mutable image must decline the hook and miss ordinary
    // strict-static dispatch. The child traps a forbidden native entry.
    const pid_t child = fork();
    check(child >= 0, "fork for negative dispatch");
    if (child == 0) {
        const rlimit no_core{0, 0};
        setrlimit(RLIMIT_CORE, &no_core);
        setenv("GBARECOMP_STRICT_STATIC", "1", 1);
        bus.write8(0x03001000u, bus.read8(0x03001000u) ^ 1u);
        g_cpu = {};
        g_cpu.cpsr = 0x3Fu;
        g_cpu.R[15] = 0x03001000u;
        g_runtime_ram_dispatch_hook = verified_ram_hook;
        g_runtime_fn_entry_hook = [](std::uint32_t pc) {
            if (pc == 0x03001000u) _exit(77);
        };
        runtime_dispatch(0x03001001u);
        _exit(78);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child, "negative dispatch wait");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "wrong RAM bytes entered private native or evaded strict miss");
    g_cpu = {};
    g_cpu.cpsr = 0x3Fu; // System, Thumb
    g_cpu.R[13] = 0x03007F00u;
    g_cpu.R[14] = 0x08000001u;
    g_cpu.R[15] = 0x03001000u;
    g_runtime_fn_entry_hook = on_entry;
    const auto start_cycle = g_runtime_cycles;
    runtime_call_push_return(0x08000000u);
    g_runtime_ram_dispatch_hook = verified_ram_hook;
    runtime_dispatch(0x03001001u);
    check(entry_pc == 0x03001000u, "entry hook PC");
    check(g_cpu.R[15] == 0x08000000u, "return PC");
    check(g_cpu.R[13] == 0x03007F00u, "stack restored");
    check(runtime_call_stack_depth() == 0, "return stack balanced");
    check(bus.read16(0x04000040u) == 0x34u, "WIN0H value");
    gba::MmioCapEntry captures[16]{};
    std::uint64_t first = 0;
    const auto count = gba::gba_mmio_cap_query(0, 16, captures, first);
    bool found = false;
    for (std::size_t i = 0; i < count; ++i) {
        if (captures[i].addr == 0x04000040u) {
            check(captures[i].pc == 0x0300100Cu, "WIN0H PC leaked ROM identity");
            check(captures[i].value == 0x34u, "WIN0H capture value");
            found = true;
        }
    }
    check(found, "WIN0H capture missing");
    const ArmCpuState relocated_cpu = g_cpu;
    const auto relocated_cycles = g_runtime_cycles - start_cycle;
    const auto relocated_stack_word = bus.read32(0x03007EFCu);
    const auto before_source_capture = gba::gba_mmio_cap_total();
    bus.write16(0x04000040u, 0u);
    g_cpu = {};
    g_cpu.cpsr = 0x3Fu;
    g_cpu.R[13] = 0x03007F00u;
    g_cpu.R[14] = 0x08000001u;
    g_cpu.R[15] = 0x08000100u;
    entry_pc = 0u;
    const auto source_start_cycle = g_runtime_cycles;
    runtime_call_push_return(0x08000000u);
    gf_source_callback();
    const auto source_cycles = g_runtime_cycles - source_start_cycle;
    check(entry_pc == 0x08000100u, "source entry identity");
    check(runtime_call_stack_depth() == 0u, "source return stack");
    for (unsigned reg = 0; reg < 15; ++reg)
        check(g_cpu.R[reg] == relocated_cpu.R[reg], "source/RAM register mismatch");
    check(g_cpu.cpsr == relocated_cpu.cpsr, "source/RAM CPSR mismatch");
    check(bus.read32(0x03007EFCu) == relocated_stack_word,
          "source/RAM stack memory mismatch");
    check(bus.read16(0x04000040u) == 0x34u, "source WIN0H value");
    const auto source_count = gba::gba_mmio_cap_query(
        before_source_capture + 1u, 16, captures, first);
    found = false;
    for (std::size_t i = 0; i < source_count; ++i) {
        if (captures[i].addr == 0x04000040u) {
            check(captures[i].pc == 0x0800010Cu, "source WIN0H PC identity");
            found = true;
        }
    }
    check(found, "source WIN0H capture missing");
    std::printf("source cycles=%llu relocated cycles=%llu\n",
                static_cast<unsigned long long>(source_cycles),
                static_cast<unsigned long long>(relocated_cycles));
    // A supported debug yield stops at an interior RAM PC. The verified
    // hook resumes the private body without a global static resume alias.
    bus.write16(0x04000040u, 0u);
    g_cpu = {};
    g_cpu.cpsr = 0x3Fu;
    g_cpu.R[13] = 0x03007F00u;
    g_cpu.R[14] = 0x08000001u;
    g_cpu.R[15] = 0x03001000u;
    const auto resume_start_cycle = g_runtime_cycles;
    runtime_call_push_return(0x08000000u);
    g_runtime_break_pc = 0x03001004u;
    runtime_dispatch(0x03001001u);
    check(g_cpu.R[15] == 0x03001004u, "private yield PC");
    check(runtime_call_stack_depth() == 1u, "yield lost return frame");
    g_runtime_break_pc = 0u;
    runtime_dispatch(0x03001005u);
    check(runtime_call_stack_depth() == 0u, "resume return stack");
    check(g_cpu.R[15] == 0x08000000u, "resume return PC");
    check(bus.read16(0x04000040u) == 0x34u, "resume WIN0H value");
    check(g_runtime_cycles - resume_start_cycle == relocated_cycles,
          "resume changed guest cycles");
    // Outer IRQ invokes the relocated callback in System mode. Its first
    // instruction tick delivers a pending nested IRQ through runtime_tick.
    g_cpu = {};
    g_cpu.cpsr = 0x3Fu;
    g_cpu.R[13] = 0x03007F00u;
    g_cpu.R[15] = 0x08000000u;
    entry_pc = 0u;
    gbarecomp::RuntimeTimingContext timing{};
    gbarecomp::runtime_capture_timing_context(timing);
    timing.vblank_starts = timing.yielded_vblank + 1u;
    gbarecomp::runtime_restore_timing_context(timing);
    runtime_irq(0x08000000u);
    check(nested_return_pc == 0x03001002u, "nested return address");
    check(g_runtime_irq_max_depth >= 2u, "nested IRQ did not run");
    check(g_irq_nest_depth == 0u, "IRQ nesting depth not restored");
    check(g_cpu.cpsr == 0x3Fu, "CPSR not restored after nested IRQ");
    check(g_cpu.R[15] == 0x08000000u, "outer IRQ return PC");
    check(runtime_call_stack_depth() == 0u, "outer IRQ return stack");
    check(entry_pc == 0x03001000u, "callback entry after nested IRQ");
    std::printf("nested IRQ: return_pc=%08x spsr=%08x max_depth=%llu\n",
                nested_return_pc, nested_spsr,
                static_cast<unsigned long long>(g_runtime_irq_max_depth));
    std::printf("private_relocation_runtime_tests: PASS cycles=%llu\n",
                static_cast<unsigned long long>(g_runtime_cycles - start_cycle));
    gbarecomp::set_active_ppu(nullptr);
    gbarecomp::set_active_bus(nullptr);
}

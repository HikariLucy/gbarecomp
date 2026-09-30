// Tail dispatch: a guest TAIL transfer must not grow the host stack, a guest
// CALL must still run the callee to completion before its continuation.
//
// The high-water is sampled at every runtime_dispatch through the force-interp
// predicate (it always answers "no", so execution stays native).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gba_bus.h"
#include "gba_io.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

struct DispatchEntry { std::uint32_t addr; std::uint8_t thumb; std::uint8_t resume; void (*fn)(); };
extern "C" const DispatchEntry kBiosDispatchTable[] = {};
extern "C" const unsigned kBiosDispatchTableLen = 0u;

namespace {
std::uintptr_t g_low;
enum class Probe { None, Yield, Irq } g_probe = Probe::None;
int g_b_entries = 0;     // calls of the predicate at B's entry PC (dispatch + prologue)
int g_a_entries = 0;
bool g_probe_fired = false;
int sample(std::uint32_t pc, int) {
    const auto here = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    if (here < g_low) g_low = here;
    if (g_probe == Probe::Yield && !g_probe_fired && pc == 0x08000100u && ++g_a_entries == 6) {
        // Arm a VBlank-style yield at the next B prologue (runtime_should_yield honours it).
        g_probe_fired = true;
        g_runtime_break_pc = 0x08000140u;
    }
    if (g_probe == Probe::Irq && !g_probe_fired && pc == 0x08000140u && ++g_b_entries == 3) {
        g_probe_fired = true;
        // IRQ-style: a nested dispatch between two tail transfers of the chain.
        const auto r14 = g_cpu.R[14];
        const auto r15 = g_cpu.R[15];
        const auto cpsr = g_cpu.cpsr;
        g_cpu.R[14] = 0x08000004u;
        runtime_call_push_return(0x08000004u);
        runtime_dispatch(0x08000280u);
        g_cpu.R[14] = r14; g_cpu.R[15] = r15; g_cpu.cpsr = cpsr;
    }
    return 0;
}
int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}
void reset(gba::GbaBus& bus, std::uint32_t limit) {
    g_cpu = {};
    g_cpu.cpsr = 0x1Fu;
    g_cpu.R[1] = limit;
    g_cpu.R[11] = 0x02000000u;           // table: [B, A, A]
    g_cpu.R[13] = 0x03007F00u;
    g_cpu.R[14] = 0x08000000u;
    bus.write32(0x02000000u, 0x08000140u);
    bus.write32(0x02000004u, 0x08000100u);
    bus.write32(0x02000008u, 0x08000100u);
    bus.write32(0x0200000Cu, 0x080002C0u);
    g_low = ~static_cast<std::uintptr_t>(0);
}
// noinline: the baseline must be run()'s own frame, not main()'s (which holds the multi-hundred-KiB bus).
__attribute__((noinline)) std::uintptr_t run(gba::GbaBus& bus, std::uint32_t entry, std::uint32_t limit) {
    reset(bus, limit);
    const std::uint32_t depth0 = runtime_call_stack_depth();
    runtime_call_push_return(0x08000000u);
    const auto base = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    runtime_dispatch(entry);
    check(runtime_call_stack_depth() == depth0, "call-return metadata is balanced");
    return base - g_low;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    // A VBlank yield unwinds the host stack by itself (and needs resume entries the
    // fixture does not have); this test measures the stack WITHOUT that relief.
    setenv("GBARECOMP_YIELD_ON_VBLANK", "0", 1);
    std::ifstream in(std::string(argv[1]) + "/fixture.gba", std::ios::binary);
    std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(in)), {});
    gba::GbaBus bus;
    gba::GbaPpu ppu;
    bus.set_rom(rom.data(), rom.size());
    gbarecomp::set_active_bus(&bus);
    gbarecomp::set_active_ppu(&ppu);
    g_runtime_force_interp_hook = sample;

    std::printf("== tail transfers (A->B->A... through runtime_dispatch)\n");
    // argv[2] optionally adds a larger run (100000 needs > 8 MiB before the fix).
    std::vector<std::uint32_t> steps = {1000u, 10000u, 30000u};
    if (argc > 2) steps.push_back(static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)));
    std::vector<std::uintptr_t> hw(steps.size());
    for (std::size_t i = 0; i < steps.size(); ++i) {
        hw[i] = run(bus, 0x08000100u, steps[i]);
        std::printf("  steps=%u high_water=%llu bytes\n", steps[i], (unsigned long long)hw[i]);
        check(g_cpu.R[0] == steps[i], "guest result: every step executed exactly once");
    }
    check(hw.back() < 256u * 1024u, "high-water for the longest tail chain < 256 KiB");
    check(hw.back() <= hw[0] + 4096u, "high-water is flat (longest vs 1000 steps within 4 KiB)");

    std::printf("== true call (bl) keeps call semantics\n");
    run(bus, 0x08000180u, 10u);
    check(g_cpu.R[3] == 1u, "callee ran to completion BEFORE the continuation");
    check(g_cpu.R[0] == 1u + 0x100u, "continuation executed exactly once after the callee");
    check(g_cpu.R[6] == 0x08000188u, "LR inside the callee is the continuation address");

    std::printf("== call whose callee performs a long tail chain\n");
    run(bus, 0x08000240u, 30000u);
    check(g_cpu.R[3] == 1u, "callee started before the continuation");
    check(g_cpu.R[5] == 30000u, "continuation observes the FINISHED tail chain");
    check(g_cpu.R[0] == 30000u, "guest result after return");

    std::printf("== yield between tail transfers, then resume\n");
    {
        g_probe = Probe::Yield; g_a_entries = 0; g_probe_fired = false;
        reset(bus, 1000u);
        runtime_call_push_return(0x08000000u);
        runtime_dispatch(0x08000100u);
        g_runtime_break_pc = 0;
        check(g_probe_fired && g_cpu.R[15] == 0x08000140u, "yield stopped at the guest PC of the next tail target");
        const std::uint32_t progress = g_cpu.R[0];
        check(progress >= 3u && progress < 1000u, "the yield interrupted the chain part-way");
        check(!g_runtime_tail_pending, "no stale tail transfer after the yield");
        g_probe = Probe::None;
        runtime_dispatch(g_cpu.R[15]);                  // the outer loop re-dispatches R15
        check(g_cpu.R[0] == 1000u, "resumed run reaches the same guest result as an uninterrupted one (no step lost or repeated)");
        check(!g_runtime_tail_pending, "no stale tail transfer after the resume");
    }

    std::printf("== nested dispatch (IRQ-style) in the middle of a chain\n");
    {
        g_probe = Probe::Irq; g_b_entries = 0; g_probe_fired = false;
        const std::uintptr_t hw_irq = run(bus, 0x08000100u, 30000u);
        g_probe = Probe::None;
        check(g_probe_fired, "the nested dispatch ran");
        check(g_cpu.R[9] == 2u, "handler ran once, including its own tail transfer");
        check(g_cpu.R[0] == 30000u, "interrupted chain still completes with the exact guest result");
        check(hw_irq < 256u * 1024u, "high-water stays bounded with a nested dispatch");
    }

    std::printf("tail_dispatch_runtime_tests: %s\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}

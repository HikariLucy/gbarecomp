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
int sample(std::uint32_t, int) {
    const auto here = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    if (here < g_low) g_low = here;
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
    g_low = ~static_cast<std::uintptr_t>(0);
}
std::uintptr_t run(gba::GbaBus& bus, std::uint32_t entry, std::uint32_t limit) {
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

    std::printf("tail_dispatch_runtime_tests: %s\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}

// Synthetic private-entry lookup scaling test (no ROM). The generated
// runtime_invoke_private_entry_in_image must not be O(table length): a hit on
// the LAST table entry may not cost materially more than a hit on the FIRST.
// Prints ns/lookup at 1k/10k/100k iterations so the cost is recorded.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "gba_bus.h"
#include "gba_io.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "recompiled.h"

extern "C" const unsigned kPrivateDispatchTableLen;

namespace {
void check(bool value, const char* why) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
std::uint64_t g_runs = 0;
void on_entry(std::uint32_t) { ++g_runs; }

// Invoke the private body at pc `n` times; returns ns per lookup+call.
double bench(std::uint32_t pc, std::uint64_t n) {
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < n; ++i) {
        g_cpu.R[14] = 0x08000001u;
        runtime_call_push_return(0x08000000u);
        if (!runtime_invoke_private_entry_in_image(0, pc, 1)) check(false, "hit declined");
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(n);
}
}  // namespace

int main(int argc, char** argv) {
    check(argc == 2, "table size (functions) required");
    const std::uint32_t count = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));
    gba::GbaBus bus;
    gba::GbaPpu ppu;
    gbarecomp::set_active_bus(&bus);
    gbarecomp::set_active_ppu(&ppu);
    g_runtime_fn_entry_hook = on_entry;
    g_cpu = {};
    g_cpu.cpsr = 0x3Fu;
    g_cpu.R[13] = 0x03007F00u;
    const std::uint32_t first_pc = 0x03001000u;
    const std::uint32_t last_pc = 0x03001000u + 2u * (count - 1u);

    // Semantics: exact image/PC/mode only, fail-closed.
    check(kPrivateDispatchTableLen >= count, "table smaller than fixture");
    check(!runtime_invoke_private_entry_in_image(0, 0x03001000u, 0), "ARM-mode alias entered Thumb entry");
    check(!runtime_invoke_private_entry_in_image(0, last_pc + 2u, 1), "PC past last entry resolved");
    check(!runtime_invoke_private_entry_in_image(0, 0x03000FFEu, 1), "PC before first entry resolved");
    check(!runtime_invoke_private_entry_in_image(0, 0x08000100u, 1), "ROM alias resolved as private");
    check(!runtime_invoke_private_entry_in_image(-1, first_pc, 1), "negative image resolved");
    check(!runtime_invoke_private_entry_in_image(99, first_pc, 1), "unknown image resolved");
    for (std::uint32_t i = 0; i < count; i += (count > 64 ? count / 61 : 1)) {
        g_runs = 0;
        g_cpu.R[14] = 0x08000001u;
        runtime_call_push_return(0x08000000u);
        check(runtime_invoke_private_entry_in_image(0, 0x03001000u + 2u * i, 1), "entry missing");
        check(g_runs == 1u, "entry ran wrong number of times");
        check(runtime_call_stack_depth() == 0u, "return stack unbalanced");
        // pc & ~1 normalisation (the Thumb-bit form of the same PC) is part of the contract.
        g_cpu.R[14] = 0x08000001u;
        runtime_call_push_return(0x08000000u);
        check(runtime_invoke_private_entry_in_image(0, 0x03001000u + 2u * i + 1u, 1), "odd-PC form");
        check(runtime_call_stack_depth() == 0u, "return stack unbalanced (odd-PC)");
    }

    // Scaling.
    double first_ns = 0, last_ns = 0;
    for (std::uint64_t n : {1000ull, 10000ull, 100000ull}) {
        first_ns = bench(first_pc, n);
        last_ns = bench(last_pc, n);
        std::printf("PRIVLOOKUP table=%u iters=%llu first_ns=%.1f last_ns=%.1f ratio=%.2f\n", count,
                    static_cast<unsigned long long>(n), first_ns, last_ns, last_ns / first_ns);
    }
    // Linear scan: last/first is ~count/2..count (>>30 at 4096). An index keeps it near 1.
    check(last_ns < first_ns * 4.0 + 50.0,
          "private lookup scales with table length (linear scan)");
    std::puts("private_lookup_runtime_tests: ok");
    return 0;
}

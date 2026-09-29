// Executes the synthetic switch: ldrne pc,[pc,r1,lsl #2] ; beq default.
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

static int failures = 0;
static void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; }
}

static std::uint32_t run_switch(gba::GbaBus& bus, std::uint32_t r0, std::uint8_t idx) {
    bus.write8(0x02000000u, idx);
    g_cpu = {};
    g_cpu.cpsr = 0x1Fu;  // System, ARM
    g_cpu.R[0] = r0;
    g_cpu.R[2] = 0x02000000u;
    g_cpu.R[13] = 0x03007F00u;
    g_cpu.R[14] = 0x08000000u;
    runtime_call_push_return(0x08000000u);
    runtime_dispatch(0x08000100u);
    check(g_cpu.R[15] == 0x08000000u, "returned to caller");
    check(runtime_call_stack_depth() == 0u, "return stack balanced");
    return g_cpu.R[0];
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream in(std::string(argv[1]) + "/fixture.gba", std::ios::binary);
    std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(in)), {});
    gba::GbaBus bus;
    gba::GbaPpu ppu;
    bus.set_rom(rom.data(), rom.size());
    gbarecomp::set_active_bus(&bus);
    gbarecomp::set_active_ppu(&ppu);
    // EQ (r0 & 9 == 0): ldrne is skipped, beq takes the default.
    check(run_switch(bus, 0u, 2) == 5u, "EQ path must take the beq default");
    // NE: the table entry selected by the byte at [r2] is the target.
    for (std::uint8_t i = 0; i < 4; ++i)
        check(run_switch(bus, 1u, i) == 0x10u + i, "NE path must jump through the table");
    check(run_switch(bus, 8u, 3) == 0x13u, "NE via bit 3");
    if (failures) return 1;
    std::printf("conditional_pc_load_runtime_tests: PASS\n");
    return 0;
}

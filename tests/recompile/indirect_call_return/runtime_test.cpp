// Executes the manual-link indirect call and its return continuation.
#include <cstdint>
#include <cstdio>
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

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream in(std::string(argv[1]) + "/fixture.gba", std::ios::binary);
    std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(in)), {});
    gba::GbaBus bus;
    gba::GbaPpu ppu;
    bus.set_rom(rom.data(), rom.size());
    gbarecomp::set_active_bus(&bus);
    gbarecomp::set_active_ppu(&ppu);
    // Pointer table at 0x02000000: entry for index r2=1 is at r11 - 4.
    bus.write32(0x02000000u, 0x08000140u);
    g_cpu = {};
    g_cpu.cpsr = 0x1Fu;
    g_cpu.R[2] = 1u;
    g_cpu.R[11] = 0x02000004u;
    g_cpu.R[13] = 0x03007F00u;
    g_cpu.R[14] = 0x08000000u;
    runtime_call_push_return(0x08000000u);
    runtime_dispatch(0x08000100u);
    if (g_cpu.R[0] != 0x21u) {  // callee +1, continuation +0x20
        std::fprintf(stderr, "FAIL: continuation after indirect call, r0=0x%X\n", g_cpu.R[0]);
        return 1;
    }
    std::printf("indirect_call_return_runtime_tests: PASS\n");
    return 0;
}

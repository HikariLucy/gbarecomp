#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "gba_bus.h"

namespace {
int failures = 0;

void check(const char* label, uint32_t got, uint32_t expected) {
    if (got != expected) {
        std::printf("FAIL %s: got %u expected %u\n", label, got, expected);
        ++failures;
    }
}

void test_power_on_and_other_regions() {
    gba::GbaBus bus;
    check("power-on register", bus.io().read16(gba::IoReg::WAITCNT), 0);
    for (uint32_t region : {0x08u, 0x0Au, 0x0Cu}) {
        const uint32_t addr = region << 24;
        check("power-on N16", bus.access_cycles(addr, 2, false), 5);
        const uint32_t seq = region == 0x0Cu ? 9u : (region == 0x0Au ? 5u : 3u);
        check("power-on S16", bus.access_cycles(addr, 2, true), seq);
        check("power-on N32", bus.access_cycles(addr, 4, false), 5u + seq);
        check("power-on S32", bus.access_cycles(addr, 4, true), 2u * seq);
    }
    check("power-on SRAM", bus.access_cycles(0x0E000000u, 1, false), 5);
    bus.io().write16(gba::IoReg::WAITCNT, 0x7FFFu);
    check("IWRAM unchanged", bus.access_cycles(0x03000000u, 4, false), 1);
    check("EWRAM unchanged", bus.access_cycles(0x02000000u, 4, false), 6);
    check("VRAM unchanged", bus.access_cycles(0x06000000u, 4, false), 2);
    bus.io().write16(gba::IoReg::WAITCNT, 0x4000u); // Hardware prefetch bit.
    check("prefetch bit stored", bus.io().read16(gba::IoReg::WAITCNT), 0x4000);
    check("prefetch has no modeled waitstate effect",
          bus.access_cycles(0x08000000u, 2, false), 5);
}

void test_rom_fields() {
    // Each pair of regions shares one independently programmed waitstate bank.
    constexpr uint32_t bases[] = {0x08u, 0x0Au, 0x0Cu};
    constexpr uint16_t shifts[] = {2, 5, 8};
    constexpr uint32_t first[] = {5, 4, 3, 9};
    for (int bank = 0; bank < 3; ++bank) {
        const uint32_t addr = bases[bank] << 24;
        for (uint16_t n = 0; n < 4; ++n) {
            for (uint16_t s = 0; s < 2; ++s) {
                gba::GbaBus bus;
                const uint16_t value = static_cast<uint16_t>((n << shifts[bank]) |
                    (s << (shifts[bank] + 2)));
                bus.io().write16(gba::IoReg::WAITCNT, value);
                const uint32_t seq = s ? 2u : (bank == 2 ? 9u : (bank == 1 ? 5u : 3u));
                check("ROM N16", bus.access_cycles(addr, 2, false), first[n]);
                check("ROM S16", bus.access_cycles(addr, 2, true), seq);
                check("ROM N32=N16+S16", bus.access_cycles(addr, 4, false), first[n] + seq);
                check("ROM S32=2*S16", bus.access_cycles(addr, 4, true), 2 * seq);
                check("ROM byte uses halfword cycle", bus.access_cycles(addr, 1, false), first[n]);
                check("mirrored bank", bus.access_cycles(addr + 0x01000000u, 2, true), seq);
                for (int other = 0; other < 3; ++other) {
                    if (other == bank) continue;
                    check("independent bank N", bus.access_cycles(bases[other] << 24, 2, false), 5);
                }
            }
        }
    }
}

void test_sram_and_dynamic_write() {
    gba::GbaBus bus;
    constexpr uint32_t sram = 0x0E000000u;
    constexpr uint32_t rom = 0x08000000u;
    check("before write ROM N", bus.access_cycles(rom, 2, false), 5);
    check("before write ROM S", bus.access_cycles(rom, 2, true), 3);
    constexpr uint32_t expected[] = {5, 4, 3, 9};
    for (uint16_t field = 0; field < 4; ++field) {
        bus.io().write16(gba::IoReg::WAITCNT, field);
        check("SRAM field", bus.access_cycles(sram, 1, false), expected[field]);
        check("SRAM sequential", bus.access_cycles(sram, 1, true), expected[field]);
    }
    bus.io().write16(gba::IoReg::WAITCNT, 0x0014u); // WS0 3/1, SRAM 4
    check("programmed register", bus.io().read16(gba::IoReg::WAITCNT), 0x0014);
    check("after write ROM N", bus.access_cycles(rom, 2, false), 4);
    check("after write ROM S", bus.access_cycles(rom, 2, true), 2);
    check("after write ROM N32", bus.access_cycles(rom, 4, false), 6);
    bus.io().write16(gba::IoReg::WAITCNT, 0x0017u); // Preserve WS0, SRAM 8
    check("SRAM helper 8 wait", bus.access_cycles(sram, 1, false), 9);
}

void test_dma_uses_current_waitstates() {
    gba::GbaBus bus;
    bus.io().set_bus(&bus);
    // Two 16-bit units: source ROM N+S, destination IWRAM N+S, startup 2.
    bus.io().write32(0xB0u, 0x08000000u);
    bus.io().write32(0xB4u, 0x03000000u);
    bus.io().write16(0xB8u, 2);
    bus.io().write16(0xBAu, 0x8000u);
    check("DMA default cycles", bus.io().take_dma_steal_cycles(), 12);
    bus.io().write16(0xBAu, 0);
    bus.io().write16(gba::IoReg::WAITCNT, 0x001Cu); // WS0 8/1
    bus.io().write16(0xBAu, 0x8000u);
    check("DMA updated cycles", bus.io().take_dma_steal_cycles(), 15);
}
}

int main() {
    test_power_on_and_other_regions();
    test_rom_fields();
    test_sram_and_dynamic_write();
    test_dma_uses_current_waitstates();
    if (failures) {
        std::printf("waitcnt_tests: %d failures\n", failures);
        return 1;
    }
    std::puts("waitcnt_tests: OK");
    return 0;
}

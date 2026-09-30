#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "gba_audio.h"
#include "gba_io.h"

namespace {

class TestBus final : public armv4t::Bus {
public:
    uint8_t read8(uint32_t addr) override { return byte(addr); }
    uint16_t read16(uint32_t addr) override {
        return static_cast<uint16_t>(read8(addr) | (read8(addr + 1) << 8));
    }
    uint32_t read32(uint32_t addr) override {
        return static_cast<uint32_t>(read8(addr)) |
               (static_cast<uint32_t>(read8(addr + 1)) << 8) |
               (static_cast<uint32_t>(read8(addr + 2)) << 16) |
               (static_cast<uint32_t>(read8(addr + 3)) << 24);
    }
    void write8(uint32_t addr, uint8_t value) override { byte(addr) = value; }
    void write16(uint32_t addr, uint16_t value) override {
        write8(addr, static_cast<uint8_t>(value));
        write8(addr + 1, static_cast<uint8_t>(value >> 8));
    }
    void write32(uint32_t addr, uint32_t value) override {
        if (addr >= 0x040000A0u && addr < 0x040000A8u) {
            fifo_writes.push_back({addr, value});
        }
        write8(addr, static_cast<uint8_t>(value));
        write8(addr + 1, static_cast<uint8_t>(value >> 8));
        write8(addr + 2, static_cast<uint8_t>(value >> 16));
        write8(addr + 3, static_cast<uint8_t>(value >> 24));
    }

    struct FifoWrite {
        uint32_t addr;
        uint32_t value;
    };
    std::vector<FifoWrite> fifo_writes;

private:
    uint8_t& byte(uint32_t addr) {
        if ((addr & 0xFF000000u) == 0x02000000u) {
            return ewram_[addr & (ewram_.size() - 1)];
        }
        return iwram_[addr & (iwram_.size() - 1)];
    }

    std::array<uint8_t, 256 * 1024> ewram_{};
    std::array<uint8_t, 32 * 1024> iwram_{};
};

int failures = 0;

void check_eq(const char* test, uint32_t got, uint32_t expected) {
    if (got != expected) {
        std::printf("FAIL %s: got 0x%08X, expected 0x%08X\n",
                    test, got, expected);
        ++failures;
    }
}

void configure_dma(gba::GbaIo& io, uint32_t source, uint32_t dest,
                   uint16_t count, uint16_t control) {
    io.write32(0xB0, source);
    io.write32(0xB4, dest);
    io.write16(0xB8, count);
    io.write16(0xBA, control);
}

void test_immediate_word_alignment() {
    TestBus bus;
    gba::GbaIo io;
    io.set_bus(&bus);
    bus.write32(0x02000000u, 0x78563412u);
    bus.write32(0x02000004u, 0xAABBCCDDu);

    configure_dma(io, 0x02000001u, 0x03000003u, 1, 0x8400u);

    check_eq("immediate_word_aligned_dest", bus.read32(0x03000000u),
             0x78563412u);
    check_eq("immediate_word_no_unaligned_write", bus.read32(0x03000004u), 0u);
    check_eq("immediate_word_run_count", static_cast<uint32_t>(io.dma_runs(0)), 1u);
    check_eq("immediate_word_unit_count", static_cast<uint32_t>(io.dma_words(0)), 1u);
}

void test_immediate_halfword_alignment() {
    TestBus bus;
    gba::GbaIo io;
    io.set_bus(&bus);
    bus.write16(0x02000010u, 0xBEEFu);
    bus.write16(0x02000012u, 0x1234u);

    configure_dma(io, 0x02000011u, 0x03000021u, 1, 0x8000u);

    check_eq("immediate_halfword_aligned_dest", bus.read16(0x03000020u),
             0xBEEFu);
    check_eq("immediate_halfword_no_unaligned_write", bus.read16(0x03000022u), 0u);
}

void test_timed_word_alignment() {
    TestBus bus;
    gba::GbaIo io;
    io.set_bus(&bus);
    bus.write32(0x02000040u, 0xDEADBEEFu);
    bus.write32(0x02000044u, 0x01020304u);

    configure_dma(io, 0x02000041u, 0x03000043u, 1, 0x9400u);
    check_eq("timed_word_not_immediate", bus.read32(0x03000040u), 0u);
    io.run_timed_dma(1);

    check_eq("timed_word_aligned_dest", bus.read32(0x03000040u), 0xDEADBEEFu);
    check_eq("timed_word_no_unaligned_write", bus.read32(0x03000044u), 0u);
}

void test_sound_fifo_dma_completion_irq() {
    TestBus bus;
    gba::GbaIo io;
    gba::GbaAudio audio;
    io.set_bus(&bus);
    io.set_audio(&audio);

    for (uint32_t i = 0; i < 8; ++i) {
        bus.write32(0x02000100u + i * 4u, 0x11111111u * (i + 1u));
    }

    // DMA2 -> FIFO B: repeat, 32-bit, special/FIFO timing, completion IRQ.
    io.write32(0xC8u, 0x02000100u);
    io.write32(0xCCu, 0x040000A4u);
    io.write16(0xD0u, 4u);
    io.write16(0xD2u, 0xF640u);

    // Timer 0 clocks both direct-sound FIFOs by default. Its first overflow
    // requests an empty FIFO refill, which is one four-word DMA burst.
    io.write16(0x100u, 0xFFFEu);
    io.write16(0x102u, 0x0080u);
    io.tick_timers(2u);

    check_eq("sound_fifo_dma_run_count_1",
             static_cast<uint32_t>(io.dma_runs(2)), 1u);
    check_eq("sound_fifo_dma_word_count_1",
             static_cast<uint32_t>(io.dma_words(2)), 4u);
    check_eq("sound_fifo_dma_irq_1",
             io.if_reg() & gba::GbaIo::IrqDma2, gba::GbaIo::IrqDma2);

    // Completion IRQ is per burst, not just the channel's initial enable.
    io.write16(gba::IoReg::IF, gba::GbaIo::IrqDma2);
    io.tick_timers(2u);
    check_eq("sound_fifo_dma_run_count_2",
             static_cast<uint32_t>(io.dma_runs(2)), 2u);
    check_eq("sound_fifo_dma_irq_2",
             io.if_reg() & gba::GbaIo::IrqDma2, gba::GbaIo::IrqDma2);
}

// ---------------------------------------------------------------------------
// Sound-FIFO DMA semantics (GBATEK "GBA DMA Transfers", "Sound DMA (FIFO)";
// mGBA GBADMAService): DMA1/DMA2 with start timing 3 serve a direct-sound FIFO
// request. The controller forces 32-bit transfers, a fixed destination and a
// count of 4 words whatever CNT_H.bit10, CNT_H.dest-control and CNT_L hold;
// the game's raw CNT_H value is left intact. A request is selected by the FIFO
// the channel's destination points at (0x040000A0 = A, 0x040000A4 = B). With
// CNT_H.repeat set the channel stays armed for later requests; with it clear
// the channel disables itself after the first burst.
constexpr uint32_t kFifoA = 0x040000A0u;
constexpr uint32_t kFifoB = 0x040000A4u;
constexpr uint32_t kSrc = 0x02000100u;

void fill_pattern(TestBus& bus, uint32_t words) {
    for (uint32_t i = 0; i < words; ++i) {
        bus.write32(kSrc + i * 4u, 0xA5000000u | (i + 1u));
    }
}

// DMA1 registers.
void arm_dma1(gba::GbaIo& io, uint32_t dest, uint16_t count, uint16_t control) {
    io.write32(0xBCu, kSrc);
    io.write32(0xC0u, dest);
    io.write16(0xC4u, count);
    io.write16(0xC6u, control);
}

// DMA2 registers.
void arm_dma2(gba::GbaIo& io, uint32_t dest, uint16_t count, uint16_t control) {
    io.write32(0xC8u, kSrc);
    io.write32(0xCCu, dest);
    io.write16(0xD0u, count);
    io.write16(0xD2u, control);
}

void start_timer0(gba::GbaIo& io) {
    io.write16(0x100u, 0xFFFEu);
    io.write16(0x102u, 0x0080u);
}

// One timer-0 overflow -> one FIFO request. (The test bus is not wired to the
// audio model, so the FIFOs stay "empty" and request again on every overflow.)
void overflow(gba::GbaIo& io) { io.tick_timers(2u); }

// A) CNT_H = 0xB200 is what the NES emulator in MZM programs: enable, special
// timing, repeat, bit10 CLEAR, dest-control 0 (increment), no IRQ.
void test_fifo_width_bit_clear_still_transfers_words() {
    TestBus bus;
    gba::GbaIo io;
    gba::GbaAudio audio;
    io.set_bus(&bus);
    io.set_audio(&audio);
    fill_pattern(bus, 16);
    arm_dma1(io, kFifoA, 4, 0xB200u);
    start_timer0(io);
    overflow(io);

    check_eq("fifo_b200_runs", static_cast<uint32_t>(io.dma_runs(1)), 1u);
    check_eq("fifo_b200_words", static_cast<uint32_t>(io.dma_words(1)), 4u);
    check_eq("fifo_b200_write_count",
             static_cast<uint32_t>(bus.fifo_writes.size()), 4u);
    for (uint32_t i = 0; i < bus.fifo_writes.size() && i < 4; ++i) {
        // 32-bit words, in source order, all landing on FIFO A (fixed dest even
        // though dest-control says "increment").
        check_eq("fifo_b200_dest", bus.fifo_writes[i].addr, kFifoA);
        check_eq("fifo_b200_data", bus.fifo_writes[i].value,
                 0xA5000000u | (i + 1u));
    }
    // The guest-visible register keeps exactly what the game wrote.
    check_eq("fifo_b200_raw_cnt_h", io.read16(0xC6u), 0xB200u);
}

// B/F) Repeat/request lifecycle and source advancement; CNT_L is ignored.
void test_fifo_repeat_lifecycle_and_count_ignored() {
    for (uint16_t cnt_l : {uint16_t{1}, uint16_t{0}, uint16_t{0x3FFF}}) {
        TestBus bus;
        gba::GbaIo io;
        gba::GbaAudio audio;
        io.set_bus(&bus);
        io.set_audio(&audio);
        fill_pattern(bus, 16);
        arm_dma1(io, kFifoA, cnt_l, 0xB640u | 0u);  // 32-bit bit set, src inc
        start_timer0(io);
        overflow(io);
        overflow(io);
        overflow(io);
        check_eq("fifo_repeat_runs", static_cast<uint32_t>(io.dma_runs(1)), 3u);
        check_eq("fifo_repeat_writes",
                 static_cast<uint32_t>(bus.fifo_writes.size()), 12u);
        for (uint32_t i = 0; i < bus.fifo_writes.size() && i < 12; ++i) {
            check_eq("fifo_repeat_src_advance", bus.fifo_writes[i].value,
                     0xA5000000u | (i + 1u));
        }
        check_eq("fifo_repeat_still_enabled", io.read16(0xC6u) & 0x8000u,
                 0x8000u);
    }
}

// Repeat clear: one burst, then the channel disables itself.
void test_fifo_no_repeat_disables_after_burst() {
    TestBus bus;
    gba::GbaIo io;
    gba::GbaAudio audio;
    io.set_bus(&bus);
    io.set_audio(&audio);
    fill_pattern(bus, 16);
    arm_dma1(io, kFifoA, 4, 0xB000u);  // enable, special, NO repeat
    start_timer0(io);
    overflow(io);
    overflow(io);
    check_eq("fifo_norepeat_runs", static_cast<uint32_t>(io.dma_runs(1)), 1u);
    check_eq("fifo_norepeat_writes",
             static_cast<uint32_t>(bus.fifo_writes.size()), 4u);
    check_eq("fifo_norepeat_disabled", io.read16(0xC6u) & 0x8000u, 0u);
}

// C) FIFO B via DMA2, with every dest-control value.
void test_fifo_b_and_dest_control_ignored() {
    for (uint16_t dest_ctrl : {0u, 1u, 2u, 3u}) {
        TestBus bus;
        gba::GbaIo io;
        gba::GbaAudio audio;
        io.set_bus(&bus);
        io.set_audio(&audio);
        fill_pattern(bus, 16);
        arm_dma2(io, kFifoB, 4,
                 static_cast<uint16_t>(0xB200u | (dest_ctrl << 5)));
        start_timer0(io);
        overflow(io);
        check_eq("fifo_b_runs", static_cast<uint32_t>(io.dma_runs(2)), 1u);
        check_eq("fifo_b_writes",
                 static_cast<uint32_t>(bus.fifo_writes.size()), 4u);
        for (uint32_t i = 0; i < bus.fifo_writes.size() && i < 4; ++i) {
            check_eq("fifo_b_dest", bus.fifo_writes[i].addr, kFifoB);
            check_eq("fifo_b_data", bus.fifo_writes[i].value,
                     0xA5000000u | (i + 1u));
        }
    }
}

// Request routing follows the destination FIFO, not the channel number: with
// FIFO A clocked by timer 1 and FIFO B by timer 0, a timer-0 overflow requests
// FIFO B only. DMA1 -> FIFO B must serve it; DMA2 -> FIFO A must not.
void test_fifo_routing_follows_destination() {
    TestBus bus;
    gba::GbaIo io;
    gba::GbaAudio audio;
    io.set_bus(&bus);
    io.set_audio(&audio);
    fill_pattern(bus, 16);
    io.write16(0x84u, 0x0080u);   // SOUNDCNT_X: master enable
    io.write16(0x82u, 0x0400u);   // FIFO A uses timer 1, FIFO B uses timer 0
    arm_dma1(io, kFifoB, 4, 0xB200u);
    arm_dma2(io, kFifoA, 4, 0xB200u);
    start_timer0(io);
    overflow(io);
    check_eq("route_dma1_to_b_runs", static_cast<uint32_t>(io.dma_runs(1)), 1u);
    check_eq("route_dma2_to_a_idle", static_cast<uint32_t>(io.dma_runs(2)), 0u);
    check_eq("route_writes", static_cast<uint32_t>(bus.fifo_writes.size()), 4u);
    if (!bus.fifo_writes.empty()) {
        check_eq("route_dest", bus.fifo_writes[0].addr, kFifoB);
    }
}

// G) Special timing only means "sound FIFO" on DMA1/DMA2. DMA0 is prohibited
// in that mode and DMA3 means video capture: neither may feed a FIFO.
void test_fifo_timing_restricted_to_dma1_dma2() {
    TestBus bus;
    gba::GbaIo io;
    gba::GbaAudio audio;
    io.set_bus(&bus);
    io.set_audio(&audio);
    fill_pattern(bus, 16);
    io.write32(0xB0u, kSrc);
    io.write32(0xB4u, kFifoA);
    io.write16(0xB8u, 4u);
    io.write16(0xBAu, 0xB600u);
    io.write32(0xD4u, kSrc);
    io.write32(0xD8u, kFifoB);
    io.write16(0xDCu, 4u);
    io.write16(0xDEu, 0xB600u);
    start_timer0(io);
    overflow(io);
    check_eq("fifo_dma0_idle", static_cast<uint32_t>(io.dma_runs(0)), 0u);
    check_eq("fifo_dma3_idle", static_cast<uint32_t>(io.dma_runs(3)), 0u);
    check_eq("fifo_dma03_no_writes",
             static_cast<uint32_t>(bus.fifo_writes.size()), 0u);
}

// D/E) Ordinary DMA keeps honouring bit 10, in immediate, VBlank and HBlank.
void test_normal_dma_width_unchanged() {
    TestBus bus;
    gba::GbaIo io;
    io.set_bus(&bus);
    bus.write32(0x02000200u, 0x44332211u);
    bus.write32(0x02000204u, 0x88776655u);
    bus.write32(0x03000200u, 0xFFFFFFFFu);
    bus.write32(0x03000204u, 0xFFFFFFFFu);
    // Immediate, 16-bit (bit10 clear): count 2 moves 4 bytes.
    configure_dma(io, 0x02000200u, 0x03000200u, 2, 0x8000u);
    check_eq("normal16_lo", bus.read32(0x03000200u), 0x44332211u);
    check_eq("normal16_untouched", bus.read32(0x03000204u), 0xFFFFFFFFu);
    // Immediate, 32-bit: count 2 moves 8 bytes.
    configure_dma(io, 0x02000200u, 0x03000300u, 2, 0x8400u);
    check_eq("normal32_w0", bus.read32(0x03000300u), 0x44332211u);
    check_eq("normal32_w1", bus.read32(0x03000304u), 0x88776655u);

    // VBlank (mode 1) and HBlank (mode 2), 16-bit with bit10 clear.
    for (uint32_t mode : {1u, 2u}) {
        uint32_t dest = 0x03000400u + mode * 0x40u;
        configure_dma(io, 0x02000200u, dest, 1,
                      static_cast<uint16_t>(0x8000u | (mode << 12)));
        io.run_timed_dma(static_cast<int>(mode));
        check_eq("timed16_data", bus.read16(dest), 0x2211u);
        check_eq("timed16_width", bus.read16(dest + 2u), 0u);
        // Non-FIFO timed channels stay non-FIFO: a sound-FIFO request must
        // not run a VBlank/HBlank channel.
        check_eq("timed_no_fifo_writes",
                 static_cast<uint32_t>(bus.fifo_writes.size()), 0u);
    }
}

// IRQ-on-end still fires per burst when the width bit is clear (0xF200).
void test_fifo_irq_with_width_bit_clear() {
    TestBus bus;
    gba::GbaIo io;
    gba::GbaAudio audio;
    io.set_bus(&bus);
    io.set_audio(&audio);
    fill_pattern(bus, 16);
    arm_dma1(io, kFifoA, 4, 0xF200u);
    start_timer0(io);
    overflow(io);
    check_eq("fifo_irq_dma1", io.if_reg() & gba::GbaIo::IrqDma1,
             gba::GbaIo::IrqDma1);
    check_eq("fifo_irq_raw_cnt_h", io.read16(0xC6u), 0xF200u);
}

}  // namespace

int main() {
    test_immediate_word_alignment();
    test_immediate_halfword_alignment();
    test_timed_word_alignment();
    test_sound_fifo_dma_completion_irq();
    test_fifo_width_bit_clear_still_transfers_words();
    test_fifo_repeat_lifecycle_and_count_ignored();
    test_fifo_no_repeat_disables_after_burst();
    test_fifo_b_and_dest_control_ignored();
    test_fifo_routing_follows_destination();
    test_fifo_timing_restricted_to_dma1_dma2();
    test_normal_dma_width_unchanged();
    test_fifo_irq_with_width_bit_clear();
    if (failures) {
        std::printf("dma_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("dma_tests: OK\n");
    return 0;
}

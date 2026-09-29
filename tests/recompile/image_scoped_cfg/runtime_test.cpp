// Runtime tests for image-scoped private CFGs (synthetic, no Nintendo bytes).
// See generate_fixture.py for the layout. Public pub_ram, overlay image A and
// overlay image B all define a Thumb body at guest 0x03001000.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gba_bus.h"
#include "gba_io.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

extern "C" std::uint32_t g_irq_nest_depth;
extern "C" std::uint32_t g_runtime_break_pc;

namespace {
int g_failures = 0;
void check(bool ok, const char* why) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", why);
    ++g_failures;
}

int g_handle_a = -1, g_handle_b = -1;
std::vector<std::uint8_t> g_img_a, g_img_b;
std::uint32_t g_irq_return_pc = 0;
int g_irq_seen = 0;

std::vector<std::uint8_t> read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

void load_ram(gba::GbaBus& bus, const std::vector<std::uint8_t>& img) {
    for (std::uint32_t i = 0; i < img.size(); ++i)
        bus.write8(0x03001000u + i, img[i]);
}

void reset_cpu() {
    g_cpu = {};
    g_cpu.cpsr = 0x3Fu;  // System, Thumb, IRQ enabled
    g_cpu.R[13] = 0x03007F00u;
    g_cpu.R[14] = 0x08000001u;
}

// Run `fn` as a callee of a synthetic caller: push a return frame so the
// body's final `bx lr` returns here instead of dispatching ROM 0x08000000.
template <typename Fn>
void run_callee(Fn&& fn) {
    reset_cpu();
    runtime_call_push_return(0x08000000u);
    fn();
}

// The game's verified RAM hook: the LIVE bytes select the overlay image; the
// GBARecomp side is handed an explicit image identity.
int verified_hook(std::uint32_t pc, int thumb) {
    if (pc < 0x03001000u || pc >= 0x03001100u || !thumb) return 0;
    auto* bus = gbarecomp::active_bus();
    auto matches = [&](const std::vector<std::uint8_t>& img) {
        for (std::uint32_t i = 0; i < img.size(); ++i)
            if (bus->read8(0x03001000u + i) != img[i]) return false;
        return true;
    };
    if (matches(g_img_a)) return runtime_invoke_private_entry_in_image(g_handle_a, pc, thumb);
    if (matches(g_img_b)) return runtime_invoke_private_entry_in_image(g_handle_b, pc, thumb);
    return 0;
}

void irq_vector() {
    ++g_irq_seen;
    g_irq_return_pc = g_cpu.R[14] - 4u;
    gbarecomp::active_bus()->write16(0x04000202u, 1u);  // ack VBlank
    runtime_exception_return(g_irq_return_pc);
}
}  // namespace

struct DispatchEntry {
    std::uint32_t addr;
    std::uint8_t thumb;
    std::uint8_t resume;
    void (*fn)();
};
extern "C" const DispatchEntry kBiosDispatchTable[] = {{0x18u, 0u, 0u, irq_vector}};
extern "C" const unsigned kBiosDispatchTableLen = 1u;

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "fixture dir required\n"); return 2; }
    const std::string dir = argv[1];
    const auto rom = read_all(dir + "/fixture.gba");
    g_img_a = read_all(dir + "/image_a.bin");
    g_img_b = read_all(dir + "/image_b.bin");
    check(rom.size() == 0x400 && g_img_a.size() == 0x100 && g_img_b.size() == 0x100,
          "fixture sizes");

    gba::GbaBus bus;
    gba::GbaPpu ppu;
    bus.set_rom(rom.data(), rom.size());
    gbarecomp::set_active_bus(&bus);
    gbarecomp::set_active_ppu(&ppu);

    g_handle_a = runtime_private_image_handle("img_a");
    g_handle_b = runtime_private_image_handle("img_b");
    check(g_handle_a > 0 && g_handle_b > 0 && g_handle_a != g_handle_b,
          "distinct positive handles for overlay images");
    check(runtime_private_image_handle("nope") < 0, "unknown image id resolves negative");
    check(runtime_private_image_handle("primary_rom") == 0, "primary is handle 0");

    // ---- public dispatch never selects an overlay ----------------------
    check(runtime_has_static_entry(0x03001000u, 1), "public 0x03001000 entry present");
    check(!runtime_has_static_entry(0x03001020u, 1), "overlay-only PC leaked to public dispatch");
    g_runtime_ram_dispatch_hook = nullptr;
    run_callee([&] {
        g_cpu.R[15] = 0x03001000u;
        runtime_dispatch(0x03001001u);
    });
    check(g_cpu.R[0] == 1u && g_cpu.R[5] == 0x51u, "ordinary dispatch must run the PUBLIC body");

    // ---- explicit overlay invocation ------------------------------------
    run_callee([&] {
        check(runtime_invoke_private_entry_in_image(g_handle_a, 0x03001000u, 1) == 1, "invoke A");
    });
    check(g_cpu.R[0] == 2u && g_cpu.R[5] == 0x1Eu && g_cpu.R[1] == 3u, "A body effects");
    run_callee([&] {
        check(runtime_invoke_private_entry_in_image(g_handle_b, 0x03001000u, 1) == 1, "invoke B");
    });
    check(g_cpu.R[0] == 3u && g_cpu.R[5] == 0x21u && g_cpu.R[3] == 0xB3B3B3B3u,
          "B body effects (reads its own literal, not A's code)");

    // Same address, other image: A has code at 0x03001020, B only data there.
    run_callee([&] {
        check(runtime_invoke_private_entry_in_image(g_handle_a, 0x03001020u, 1) == 1, "A f2");
    });
    check(g_cpu.R[3] == 0xA3u, "A f2 effect");
    reset_cpu();
    check(runtime_invoke_private_entry_in_image(g_handle_b, 0x03001020u, 1) == 0,
          "B literal word must not be an entry");

    // A -> public callee (outside A's range) resolves to the public body.
    run_callee([&] {
        check(runtime_invoke_private_entry_in_image(g_handle_a, 0x03001040u, 1) == 1, "A f3");
    });
    check(g_cpu.R[4] == 7u, "A -> public q call");

    // ---- interior resume aliases are per image --------------------------
    for (int who = 0; who < 2; ++who) {
        run_callee([&] {
            g_cpu.R[0] = 0x77u;
            check(runtime_invoke_private_entry_in_image(who ? g_handle_b : g_handle_a,
                                                        0x03001004u, 1) == 1,
                  "resume at 0x03001004");
        });
        check(g_cpu.R[0] == 0x77u && g_cpu.R[5] == (who ? 0x21u : 0x1Eu),
              "resume must execute the selected image's interior");
    }
    check(!runtime_has_static_entry(0x03001004u, 1), "private resume leaked to public dispatch");

    // ---- fail closed -----------------------------------------------------
    reset_cpu();
    check(runtime_invoke_private_entry_in_image(-1, 0x03001000u, 1) == 0, "unknown handle -1");
    check(runtime_invoke_private_entry_in_image(99, 0x03001000u, 1) == 0, "unknown handle 99");
    check(runtime_invoke_private_entry_in_image(g_handle_a, 0x03001000u, 0) == 0, "wrong mode");
    check(runtime_invoke_private_entry_in_image(g_handle_a, 0x03001080u, 1) == 0, "no entry PC");
    check(runtime_invoke_private_entry_in_image(0, 0x03001000u, 1) == 0,
          "handle 0 (primary world) must not reach overlay bodies");
    check(runtime_invoke_private_entry(0x03001000u, 1) == 0,
          "legacy unscoped API must not select an overlay");

    // ---- yield / resume identity ----------------------------------------
    g_runtime_ram_dispatch_hook = verified_hook;
    for (int who = 0; who < 2; ++who) {
        load_ram(bus, who ? g_img_b : g_img_a);
        run_callee([&] {
            g_runtime_break_pc = 0x03001004u;
            g_cpu.R[15] = 0x03001000u;
            runtime_dispatch(0x03001001u);
            g_runtime_break_pc = 0;
            check(g_cpu.R[15] == 0x03001004u && g_cpu.R[5] == 0u,
                  "yield at interior PC");
            // The outer loop re-dispatches the yielded PC; the hook must
            // re-select the SAME image from live bytes.
            runtime_dispatch(g_cpu.R[15]);
        });
        check(g_cpu.R[0] == (who ? 3u : 2u) && g_cpu.R[5] == (who ? 0x21u : 0x1Eu),
              "resume after yield returned to the wrong image");
    }

    // ---- IRQ inside an overlay body --------------------------------------
    for (int who = 0; who < 2; ++who) {
        load_ram(bus, who ? g_img_b : g_img_a);
        g_irq_seen = 0;
        run_callee([&] {
            bus.write16(0x04000200u, 1u);  // IE: VBlank
            bus.write16(0x04000208u, 1u);  // IME
            bus.io().request_irq(1u);
            g_cpu.R[15] = 0x03001000u;
            runtime_dispatch(0x03001001u);
        });
        check(g_irq_seen >= 1, "IRQ delivered during overlay body");
        check(g_irq_return_pc >= 0x03001000u && g_irq_return_pc < 0x03001010u,
              "IRQ return PC lies in the overlay body");
        check(g_cpu.R[5] == (who ? 0x21u : 0x1Eu),
              "overlay body finished with its own effects after IRQ");
    }

    // ---- finally: no hook, ordinary dispatch is still the public body ----
    g_runtime_ram_dispatch_hook = nullptr;
    run_callee([&] {
        g_cpu.R[15] = 0x03001000u;
        runtime_dispatch(0x03001001u);
    });
    check(g_cpu.R[0] == 1u, "ordinary dispatch still public after overlay runs");

    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::printf("image_scoped_cfg_runtime_tests: PASS\n");
    return 0;
}

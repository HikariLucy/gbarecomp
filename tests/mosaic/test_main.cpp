#include "gba_ppu.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

void put16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

struct Fixture {
    std::array<uint8_t, 0x400> io{};
    std::array<uint8_t, 0x18000> vram{};
    std::array<uint8_t, 0x400> oam{};
    std::array<uint8_t, 0x400> pal{};
    std::array<uint8_t, gba::GbaPpu::kFramebufferBytes> rgb{};
    gba::GbaPpu ppu;

    Fixture() {
        for (int n = 0; n < 128; ++n) put16(&oam[n * 8], 0x0200);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                vram[y * 8 + x] = static_cast<uint8_t>(1 + (x + y) % 3);
        put16(&pal[2], 0x001F);  // red
        put16(&pal[4], 0x03E0);  // green
        put16(&pal[6], 0x7C00);  // blue
        put16(&pal[0x202], 0x001F);
        put16(&pal[0x204], 0x03E0);
        put16(&pal[0x206], 0x7C00);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                vram[0x10000 + y * 8 + x] = static_cast<uint8_t>(1 + (x + y) % 3);
    }

    void bg(bool enabled = true) {
        put16(&io[0x08], static_cast<uint16_t>(0x0180 | (enabled ? 0x0040 : 0)));
        put16(&vram[0x800], 0);
    }
    void obj(bool enabled = true, uint16_t extras = 0) {
        put16(&oam[0], static_cast<uint16_t>(0x2000 | extras | (enabled ? 0x1000 : 0)));
        put16(&oam[2], 0);
        put16(&oam[4], 0);
    }
    void draw(uint16_t dispcnt) {
        ppu.render(rgb.data(), dispcnt, io.data(), vram.data(), oam.data(), pal.data());
    }
    bool pixel(int x, int y, int r, int g, int b, const char* label) const {
        const auto* p = &rgb[(y * 240 + x) * 3];
        if (p[0] == r && p[1] == g && p[2] == b) return true;
        std::fprintf(stderr, "%s (%d,%d): expected %d,%d,%d got %u,%u,%u\n",
                     label, x, y, r, g, b, p[0], p[1], p[2]);
        return false;
    }
};

bool run(const char* name) {
    Fixture f;
    if (std::strcmp(name, "bg_horizontal") == 0) {
        f.bg(); put16(&f.io[0x4C], 0x0001); f.draw(0x0100);
        return f.pixel(1, 0, 255, 0, 0, name) && f.pixel(2, 0, 0, 0, 255, name);
    }
    if (std::strcmp(name, "bg_vertical") == 0) {
        f.bg(); put16(&f.io[0x4C], 0x0010); f.draw(0x0100);
        return f.pixel(0, 1, 255, 0, 0, name) && f.pixel(0, 2, 0, 0, 255, name);
    }
    if (std::strcmp(name, "bg_disabled") == 0) {
        f.bg(false); put16(&f.io[0x4C], 0x0011); f.draw(0x0100);
        return f.pixel(1, 0, 0, 255, 0, name) && f.pixel(0, 1, 0, 255, 0, name);
    }
    if (std::strcmp(name, "obj_horizontal") == 0) {
        f.obj(); put16(&f.io[0x4C], 0x0100); f.draw(0x1040);
        return f.pixel(1, 0, 255, 0, 0, name) && f.pixel(2, 0, 0, 0, 255, name);
    }
    if (std::strcmp(name, "obj_vertical") == 0) {
        f.obj(); put16(&f.io[0x4C], 0x1000); f.draw(0x1040);
        return f.pixel(0, 1, 255, 0, 0, name) && f.pixel(0, 2, 0, 0, 255, name);
    }
    if (std::strcmp(name, "obj_disabled") == 0) {
        f.obj(false); put16(&f.io[0x4C], 0x1100); f.draw(0x1040);
        return f.pixel(1, 0, 0, 255, 0, name) && f.pixel(0, 1, 0, 255, 0, name);
    }
    if (std::strcmp(name, "size_one") == 0) {
        f.bg(); f.obj(); put16(&f.io[0x4C], 0); f.draw(0x1140);
        return f.pixel(1, 0, 0, 255, 0, name) && f.pixel(0, 1, 0, 255, 0, name);
    }
    if (std::strcmp(name, "mixed_sizes") == 0) {
        f.bg(); put16(&f.io[0x4C], 0x0001 | 0x0020); f.draw(0x0100);
        return f.pixel(1, 1, 255, 0, 0, name) && f.pixel(2, 3, 0, 0, 255, name);
    }
    if (std::strcmp(name, "affine_bg") == 0) {
        put16(&f.io[0x0C], 0x0140); // BG2 map 1, mosaic enabled.
        put16(&f.io[0x20], 0x0200); // PA = 2.0: x1 normally samples texel 2.
        put16(&f.io[0x26], 0x0200); // PD = 2.0: y1 normally samples texel 2.
        f.vram[0x800] = 0;
        put16(&f.io[0x4C], 0x0011);
        f.draw(0x0401);
        return f.pixel(1, 1, 255, 0, 0, name);
    }
    if (std::strcmp(name, "obj_hflip") == 0) {
        f.obj(); put16(&f.oam[2], 0x1000); put16(&f.io[0x4C], 0x0100);
        f.draw(0x1040);
        // Source texel 7 is green; screen x1 repeats screen x0 after flipping.
        return f.pixel(1, 0, 0, 255, 0, name);
    }
    if (std::strcmp(name, "obj_affine") == 0) {
        f.obj(true, 0x0100);
        put16(&f.oam[6], 0x0100); put16(&f.oam[14], 0);
        put16(&f.oam[22], 0); put16(&f.oam[30], 0x0100);
        put16(&f.io[0x4C], 0x0100); f.draw(0x1040);
        return f.pixel(1, 0, 255, 0, 0, name);
    }
    if (std::strcmp(name, "bitmap_bg") == 0) {
        put16(&f.io[0x0C], 0x0040); // Mode 3 BG2 mosaic.
        put16(&f.io[0x4C], 0x0001);
        put16(&f.io[0x20], 0x0100); put16(&f.io[0x26], 0x0100);
        put16(&f.vram[0], 0x001F); put16(&f.vram[2], 0x03E0);
        f.draw(0x0403);
        return f.pixel(1, 0, 255, 0, 0, name);
    }
    if (std::strcmp(name, "obj_window") == 0) {
        f.bg(false);
        f.obj(true, 0x0800); // OBJ-window stencil, mosaic enabled.
        f.vram[0x10001] = 0; // x1 transparent before mosaic.
        put16(&f.io[0x4C], 0x0100);
        put16(&f.io[0x4A], 0x0100); // Outside: no layers. OBJ window: BG0.
        f.draw(0x9140);
        return f.pixel(1, 0, 0, 255, 0, name);
    }
    if (std::strcmp(name, "blend_after_mosaic") == 0) {
        f.bg(false); f.obj(true, 0x0400); // semi-transparent OBJ
        put16(&f.io[0x4C], 0x0100);
        put16(&f.io[0x50], 0x0100); // BG0 second target
        put16(&f.io[0x52], 0x0808); // half OBJ, half BG
        // Make OBJ x0 blue, x1 red, while BG x0 red, x1 green.
        f.vram[0x10000] = 3; f.vram[0x10001] = 1;
        f.draw(0x1140);
        return f.pixel(1, 0, 0, 123, 132, name);
    }
    std::fprintf(stderr, "unknown mosaic case: %s\n", name);
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (!run(argv[1])) return 1;
    std::printf("mosaic_%s: PASS\n", argv[1]);
    return 0;
}

#include "config.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

bool write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

std::string base_config(const std::string& hooks) {
    return
        "[program]\n"
        "load_address = 0x08000000\n"
        "size = 0x100\n"
        "entry_pc = 0x08000000\n\n"
        "[identity]\n"
        "sha1 = \"0000000000000000000000000000000000000000\"\n\n" + hooks;
}

}  // namespace

int main() {
    const fs::path path = fs::temp_directory_path() /
        ("gbarecomp-config-hook-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".toml");
    std::error_code ec;
    gbarecomp::Config cfg;

    const std::string valid = base_config(
        "[[mod_function_hook]]\n"
        "addr = 0x08000020\n"
        "mode = \"thumb\"\n");
    if (!write_file(path, valid) || !gbarecomp::load_config(path.string(), cfg) ||
        cfg.mod_function_hooks.size() != 1 ||
        cfg.mod_function_hooks[0].mode != gbarecomp::CpuMode::Thumb) {
        fs::remove(path, ec);
        return fail("valid mod_function_hook did not parse");
    }

    const std::string private_relocated = base_config(
        "[[extra_func]]\n"
        "addr = 0x03001000\n"
        "source_addr = 0x08000020\n"
        "mode = \"thumb\"\n"
        "dispatch = false\n");
    if (!write_file(path, private_relocated) ||
        !gbarecomp::load_config(path.string(), cfg) ||
        cfg.extra_funcs.size() != 1 ||
        !cfg.extra_funcs[0].private_entry ||
        cfg.extra_funcs[0].source_addr != 0x08000020u) {
        fs::remove(path, ec);
        return fail("private relocated extra_func did not parse");
    }

    const std::string nonreturn = base_config(
        "[[extra_func]]\n"
        "addr = 0x08000020\n"
        "mode = \"arm\"\n"
        "returns = false\n");
    if (!write_file(path, nonreturn) ||
        !gbarecomp::load_config(path.string(), cfg) ||
        cfg.extra_funcs.size() != 1 ||
        !cfg.extra_funcs[0].returns_declared || cfg.extra_funcs[0].returns) {
        fs::remove(path, ec);
        return fail("explicit non-returning function did not parse");
    }
    const std::string invalid_returns = base_config(
        "[[extra_func]]\n"
        "addr = 0x08000020\n"
        "mode = \"arm\"\n"
        "returns = \"false\"\n");
    if (!write_file(path, invalid_returns) ||
        gbarecomp::load_config(path.string(), cfg)) {
        fs::remove(path, ec);
        return fail("non-boolean returns was accepted");
    }

    const std::string private_resume = base_config(
        "[[extra_func]]\n"
        "addr = 0x03001002\n"
        "source_addr = 0x08000022\n"
        "mode = \"thumb\"\n"
        "resume = true\n"
        "dispatch = false\n");
    if (!write_file(path, private_resume) ||
        gbarecomp::load_config(path.string(), cfg)) {
        fs::remove(path, ec);
        return fail("private resume alias was accepted without a private host");
    }

    const std::string unaligned = base_config(
        "[[mod_function_hook]]\n"
        "addr = 0x08000021\n"
        "mode = \"thumb\"\n");
    if (!write_file(path, unaligned) || gbarecomp::load_config(path.string(), cfg)) {
        fs::remove(path, ec);
        return fail("unaligned mod_function_hook was accepted");
    }

    const std::string mismatch = base_config(
        "[[extra_func]]\n"
        "addr = 0x08000020\n"
        "mode = \"arm\"\n\n"
        "[[mod_function_hook]]\n"
        "addr = 0x08000020\n"
        "mode = \"thumb\"\n");
    if (!write_file(path, mismatch) || gbarecomp::load_config(path.string(), cfg)) {
        fs::remove(path, ec);
        return fail("mode-conflicting mod_function_hook was accepted");
    }
    fs::remove(path, ec);
    return 0;
}

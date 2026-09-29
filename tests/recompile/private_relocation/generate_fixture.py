#!/usr/bin/env python3
"""Generate a synthetic Thumb callback and private relocated translation."""
import hashlib
import pathlib
import subprocess
import sys

compiler = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
root.mkdir(parents=True, exist_ok=True)
rom = bytearray(0x200)
rom[0:4] = (0xE12FFF1E).to_bytes(4, 'little')  # ARM bx lr entry
# push lr; two RAM-relative literal loads; internal branch; WIN0H write; pop pc.
for offset, opcode in ((0x100, 0xB500), (0x102, 0x4807),
                       (0x104, 0x4907), (0x106, 0x2234),
                       (0x108, 0xE000), (0x10A, 0x22FF),
                       (0x10C, 0x8002), (0x10E, 0xBD00)):
    rom[offset:offset + 2] = opcode.to_bytes(2, 'little')
rom[0x120:0x124] = (0x04000040).to_bytes(4, 'little')
rom[0x124:0x128] = (0x02000000).to_bytes(4, 'little')
rom_path = root / 'fixture.gba'
rom_path.write_bytes(rom)
config = root / 'fixture.toml'
config.write_text(f'''[program]
load_address = 0x08000000
size = 0x200
entry_pc = 0x08000000
speculative_literal_harvest = false
codegen_shards = 2
[identity]
sha1 = "{hashlib.sha1(rom).hexdigest()}"
[[extra_func]]
addr = 0x03001000
source_addr = 0x08000100
mode = "thumb"
name = "private_ram_callback"
dispatch = false
[[extra_func]]
addr = 0x08000100
mode = "thumb"
name = "source_callback"
''')
out = root / 'generated'
subprocess.run((str(compiler), '--rom', str(rom_path), '--config', str(config),
                '--out', str(out), '--max-functions', '50'), check=True,
               stdout=subprocess.DEVNULL)
header = (out / 'recompiled.h').read_text()
table = (out / 'dispatch_table.cpp').read_text()
public_table, private_table = table.split('kPrivateDispatchTable[]', 1)
body = ''.join(path.read_text() for path in out.glob('recompiled_*.cpp'))
assert 'gf_private_ram_callback(void)' in header
assert 'gf_private_ram_callback(void)' in body
assert '0x03001000u' not in public_table and '0x0300100Cu' not in public_table
assert '0x03001000u' in private_table and '0x03001004u' in private_table
assert '0x0300100Au' not in private_table  # skipped branch padding
assert '0x03001020u' not in private_table  # literal pool
assert 'g_runtime_fn_entry_hook(0x03001000u)' in body
assert '03001008 T b 0x0300100c' in body
assert '0x03001006u & ~3u' in body
assert 'RUNTIME_TRACE_MEM_WRITE, 0x0300100Cu' in body

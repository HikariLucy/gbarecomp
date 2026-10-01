#!/usr/bin/env python3
"""Synthetic ROM with N private relocated Thumb leaf functions (no game ROM).

Each function is `bx lr` (0x4770) copied from ROM to IWRAM 0x03001000+2*i via a
private extra_func, so the generated private dispatch table has N entries.
"""
import hashlib
import pathlib
import subprocess
import sys

compiler = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
count = int(sys.argv[3]) if len(sys.argv) > 3 else 4096
root.mkdir(parents=True, exist_ok=True)
size = 0x200 + 2 * count
rom = bytearray(size)
rom[0:4] = (0xE12FFF1E).to_bytes(4, 'little')  # ARM bx lr entry
for i in range(count):
    rom[0x100 + 2 * i:0x102 + 2 * i] = (0x4770).to_bytes(2, 'little')
rom_path = root / 'fixture.gba'
rom_path.write_bytes(rom)
config = root / 'fixture.toml'
parts = [f'''[program]
load_address = 0x08000000
size = {size:#x}
entry_pc = 0x08000000
speculative_literal_harvest = false
codegen_shards = 2
[identity]
sha1 = "{hashlib.sha1(rom).hexdigest()}"
''']
for i in range(count):
    parts.append(f'''[[extra_func]]
addr = {0x03001000 + 2 * i:#010x}
source_addr = {0x08000100 + 2 * i:#010x}
mode = "thumb"
name = "private_leaf_{i}"
dispatch = false
''')
config.write_text(''.join(parts))
out = root / 'generated'
subprocess.run((str(compiler), '--rom', str(rom_path), '--config', str(config),
                '--out', str(out), '--max-functions', str(count + 50)),
               check=True, stdout=subprocess.DEVNULL)
table = (out / 'dispatch_table.cpp').read_text()
assert 'kPrivateDispatchTable[]' in table

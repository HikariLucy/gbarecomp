#!/usr/bin/env python3
"""Synthetic ARM `ldrne pc,[pc,Rn,lsl #2]` + `beq` switch with an inline table."""
import hashlib
import pathlib
import re
import subprocess
import sys

compiler, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
root.mkdir(parents=True, exist_ok=True)


def arm(*ops):
    return b''.join(o.to_bytes(4, 'little') for o in ops)


rom = bytearray(0x200)
rom[0:4] = arm(0xE12FFF1E)
rom[0x100:0x110] = arm(
    0xE3100009,   # tst   r0,#9
    0xE5D21000,   # ldrb  r1,[r2]
    0x179FF101,   # ldrne pc,[pc,r1,lsl #2]   (table @ 0x08000110)
    0x0A000007)   # beq   0x08000130
rom[0x110:0x120] = arm(0x08000140, 0x08000150, 0x08000160, 0x08000170)  # inline table
rom[0x130:0x138] = arm(0xE3A00005, 0xE12FFF1E)   # default: mov r0,#5 ; bx lr
for i, addr in enumerate((0x140, 0x150, 0x160, 0x170)):
    rom[addr:addr + 8] = arm(0xE3A00010 + i, 0xE12FFF1E)
(root / 'fixture.gba').write_bytes(rom)
(root / 'fixture.toml').write_text(f'''[program]
load_address = 0x08000000
size = 0x200
entry_pc = 0x08000000
speculative_literal_harvest = false
codegen_shards = 2
[identity]
sha1 = "{hashlib.sha1(rom).hexdigest()}"
[[extra_func]]
addr = 0x08000100
mode = "arm"
name = "sw"
[[jump_table]]
addr = 0x08000110
stride = 4
count = 4
format = "abs32"
entries_mode = "arm"
name = "sw_table"
''')
out = root / 'generated'
subprocess.run((str(compiler), '--rom', str(root / 'fixture.gba'), '--config',
                str(root / 'fixture.toml'), '--out', str(out),
                '--max-functions', '50'), check=True, stdout=subprocess.DEVNULL)
header = (out / 'recompiled.h').read_text()
table = (out / 'dispatch_table.cpp').read_text()
body = ''.join(p.read_text() for p in sorted(out.glob('recompiled_*.cpp')))
funcs = re.findall(r'void (gf_\w+)\(void\);\s+/\* 0x([0-9A-F]{8}) arm', header)
addrs = {a for _, a in funcs}
# The switch, the default and the four table targets are generated ...
for a in ('08000100', '08000130', '08000140', '08000150', '08000160', '08000170'):
    assert a in addrs, (a, sorted(addrs))
# ... but neither the beq as a separate root nor any table byte is code.
for a in ('0800010C', '08000110', '08000114', '08000118', '0800011C'):
    assert a not in addrs, a
assert '0x08000110u' not in table and '0x0800010Cu' not in table
print('fixture generated')

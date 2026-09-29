#!/usr/bin/env python3
"""Synthetic ARM manual-link indirect call: add lr,pc,#0 ; ldr pc,[rN,-rM,lsl #2].

The callee returns with `bx lr` to the instruction after the ldr. That
continuation must be a generated entry (it is reached only through LR).
"""
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
# caller @0x100:
#   mov   r4,lr               ; keep the real return address
#   add   lr,pc,#0            ; lr = 0x0800010C
#   ldr   pc,[r11,-r2,lsl #2] ; indirect call through a pointer table in RAM
#   (0x10C) add r0,r0,#0x20   ; continuation, reached only via the callee's bx lr
#   mov   lr,r4 ; bx lr
rom[0x100:0x118] = arm(0xE1A0400E, 0xE28FE000, 0xE71BF102, 0xE2800020, 0xE1A0E004, 0xE12FFF1E)
# callee @0x140: add r0,r0,#1 ; bx lr
rom[0x140:0x148] = arm(0xE2800001, 0xE12FFF1E)
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
name = "caller"
[[extra_func]]
addr = 0x08000140
mode = "arm"
name = "callee"
''')
out = root / 'generated'
subprocess.run((str(compiler), '--rom', str(root / 'fixture.gba'), '--config',
                str(root / 'fixture.toml'), '--out', str(out),
                '--max-functions', '50'), check=True, stdout=subprocess.DEVNULL)
header = (out / 'recompiled.h').read_text()
addrs = set(re.findall(r'/\* 0x([0-9A-F]{8}) arm', header))
assert '0800010C' in addrs, sorted(addrs)  # the LR continuation is an entry
print('fixture generated')

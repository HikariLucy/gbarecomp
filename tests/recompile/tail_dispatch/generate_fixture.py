#!/usr/bin/env python3
"""Synthetic ROM for the tail-dispatch tests (no game bytes).

A @0x100 / B @0x140  add r0,r0,#1 ; cmp r0,r1 ; ldrlt pc,[r11,#0|#4] ; bx lr
    indirect tail transfers A->B->A... until r0 reaches r1, then a registered
    `bx lr` return (the TAIL case).
caller  @0x180  mov r4,lr ; bl callee ; mov r3,r2 ; add r0,r0,#0x100 ; mov lr,r4 ; bx lr
callee  @0x1C0  mov r2,#1 ; add r0,r0,#1 ; mov r6,lr ; bx lr
    a real CALL: the callee must finish before the continuation (r3 == 1).
caller2 @0x240  mov r4,lr ; bl callee2 ; mov r3,r2 ; mov r5,r0 ; mov lr,r4 ; bx lr
callee2 @0x200  mov r2,#1 ; ldr pc,[r11,#8]     (-> A)
    a CALL whose callee performs thousands of TAIL transfers before it returns:
    the continuation must observe the finished chain (r5 == r1).
irq_h @0x280 / irq_h2 @0x2C0: a handler run by a nested dispatch in the middle of a
chain; it tail-transfers once and returns (r9 counts its instructions).
"""
import hashlib
import pathlib
import subprocess
import sys

compiler, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
root.mkdir(parents=True, exist_ok=True)


def arm(*ops):
    return b''.join(o.to_bytes(4, 'little') for o in ops)


def bl(at, to):
    return 0xEB000000 | (((to - at - 8) // 4) & 0x00FFFFFF)


rom = bytearray(0x300)
rom[0:4] = arm(0xE12FFF1E)
rom[0x100:0x110] = arm(0xE2800001, 0xE1500001, 0xB59BF000, 0xE12FFF1E)
rom[0x140:0x150] = arm(0xE2800001, 0xE1500001, 0xB59BF004, 0xE12FFF1E)
rom[0x180:0x198] = arm(0xE1A0400E, bl(0x184, 0x1C0), 0xE1A03002, 0xE2800C01, 0xE1A0E004, 0xE12FFF1E)
rom[0x1C0:0x1D0] = arm(0xE3A02001, 0xE2800001, 0xE1A0600E, 0xE12FFF1E)
rom[0x200:0x208] = arm(0xE3A02001, 0xE59BF008)
rom[0x240:0x258] = arm(0xE1A0400E, bl(0x244, 0x200), 0xE1A03002, 0xE1A05000, 0xE1A0E004, 0xE12FFF1E)
# irq_h @0x280: add r9,r9,#1 ; ldr pc,[r11,#12]   (a handler that itself tail-transfers)
# irq_h2 @0x2C0: add r9,r9,#1 ; bx lr
rom[0x280:0x288] = arm(0xE2899001, 0xE59BF00C)
rom[0x2C0:0x2C8] = arm(0xE2899001, 0xE12FFF1E)
(root / 'fixture.gba').write_bytes(rom)
funcs = [(0x100, 'chain_a'), (0x140, 'chain_b'), (0x180, 'caller'), (0x1C0, 'callee'),
         (0x200, 'callee2'), (0x240, 'caller2'),
         (0x280, 'irq_h'), (0x2C0, 'irq_h2')]
extra = ''.join(f'[[extra_func]]\naddr = 0x{0x08000000 + a:08X}\nmode = "arm"\nname = "{n}"\n' for a, n in funcs)
(root / 'fixture.toml').write_text(f'''[program]
load_address = 0x08000000
size = 0x300
entry_pc = 0x08000000
speculative_literal_harvest = false
codegen_shards = 2
[identity]
sha1 = "{hashlib.sha1(rom).hexdigest()}"
{extra}''')
subprocess.run((str(compiler), '--rom', str(root / 'fixture.gba'), '--config',
                str(root / 'fixture.toml'), '--out', str(root / 'generated'),
                '--max-functions', '50'), check=True, stdout=subprocess.DEVNULL)
print('fixture generated')

#!/usr/bin/env python3
"""Synthetic overlapping-executable-image fixture (no Nintendo bytes).

One public primary function and two private overlay images all live at guest
0x03001000 (Thumb):

  primary ROM 0x08000100 --code_copy--> 0x03001000  pub_ram   (public entry)
  image A (overlay)  load 0x03001000                a_root    (private)
  image B (overlay)  load 0x03001000                b_root    (private)

Distinguishing effects (r0 marker, r5 = 3 loop passes of a per-image addend):
  public: r0=1 r5=0x51      A: r0=2 r5=0x1E      B: r0=3 r5=0x21 r3=0xB3B3B3B3
Image A also has code at 0x03001020 (a_f2) and calls public q (0x03002000);
image B has a literal word at 0x03001020 declared as B-scoped data.
"""
import hashlib
import pathlib
import subprocess
import sys

compiler = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
root.mkdir(parents=True, exist_ok=True)


def thumb(*ops):
    return b''.join(o.to_bytes(2, 'little') for o in ops)


rom = bytearray(0x400)
rom[0:4] = (0xE12FFF1E).to_bytes(4, 'little')  # ARM bx lr entry
# public pub_ram: movs r0,#1; movs r5,#0x50; adds r5,#1; bx lr
rom[0x100:0x108] = thumb(0x2001, 0x2550, 0x3501, 0x4770)
# public q at runtime 0x03002000 (ROM 0x140): movs r4,#7; bx lr
rom[0x140:0x144] = thumb(0x2407, 0x4770)


def image(marker, addend, with_literal):
    img = bytearray(0x100)
    # movs r0,#marker; movs r5,#0; L: adds r5,#addend; adds r1,#1; cmp r1,#3;
    # bne L; <tail>
    body = thumb(0x2000 | marker, 0x2500, 0x3500 | addend, 0x3101, 0x2903, 0xD1FB)
    img[0:len(body)] = body
    if with_literal:
        # ldr r3,[pc,#0x10] -> literal at +0x20 ; bx lr
        tail = thumb(0x4B04, 0x4770)
        img[0x0C:0x0C + len(tail)] = tail
        img[0x20:0x24] = (0xB3B3B3B3).to_bytes(4, 'little')
    else:
        img[0x0C:0x0E] = thumb(0x4770)
        # a_f2 at +0x20: movs r3,#0xA3; bx lr
        img[0x20:0x24] = thumb(0x23A3, 0x4770)
        # a_f3 at +0x40: push {lr}; bl 0x03002000; pop {pc}
        # BL from 0x03001042: offset = 0x03002000 - (0x03001042 + 4)
        off = 0x03002000 - (0x03001042 + 4)
        hi = 0xF000 | ((off >> 12) & 0x7FF)
        lo = 0xF800 | ((off >> 1) & 0x7FF)
        img[0x40:0x48] = thumb(0xB500, hi, lo, 0xBD00)
    # Tail transfer inside the image: hop @+0x60 `bx r7` (r7 = 0x03001081), dest @+0x80
    # `movs r4,#0xA4|0xB4 ; bx lr`. Both images hold it at the same guest address.
    img[0x60:0x62] = thumb(0x4738)
    img[0x80:0x84] = thumb(0x2400 | (0xB4 if with_literal else 0xA4), 0x4770)
    return img


img_a = image(2, 0x0A, False)
img_b = image(3, 0x0B, True)
(root / 'image_a.bin').write_bytes(img_a)
(root / 'image_b.bin').write_bytes(img_b)
rom_path = root / 'fixture.gba'
rom_path.write_bytes(rom)


def sha(b):
    return hashlib.sha256(b).hexdigest()


config = root / 'fixture.toml'
config.write_text(f'''[program]
load_address = 0x08000000
size = 0x400
entry_pc = 0x08000000
speculative_literal_harvest = false
codegen_shards = 2
[identity]
sha1 = "{hashlib.sha1(rom).hexdigest()}"

[[code_copy]]
runtime_start = 0x03001000
source_start = 0x08000100
size = 0x10
name = "pub_ram_copy"
[[code_copy]]
runtime_start = 0x03002000
source_start = 0x08000140
size = 0x10
name = "q_copy"

[[extra_func]]
addr = 0x03001000
mode = "thumb"
name = "pub_ram"
[[extra_func]]
addr = 0x03002000
mode = "thumb"
name = "pub_q"

[[executable_image]]
id = "img_a"
path = "{root / 'image_a.bin'}"
load_address = 0x03001000
size = 0x100
sha256 = "{sha(img_a)}"
overlay = true
[[executable_image]]
id = "img_b"
path = "{root / 'image_b.bin'}"
load_address = 0x03001000
size = 0x100
sha256 = "{sha(img_b)}"
overlay = true

[[extra_func]]
addr = 0x03001000
mode = "thumb"
name = "a_root"
dispatch = false
image = "img_a"
[[extra_func]]
addr = 0x03001020
mode = "thumb"
name = "a_f2"
dispatch = false
image = "img_a"
[[extra_func]]
addr = 0x03001040
mode = "thumb"
name = "a_f3"
dispatch = false
image = "img_a"
[[extra_func]]
addr = 0x03001060
mode = "thumb"
name = "a_hop"
dispatch = false
image = "img_a"
[[extra_func]]
addr = 0x03001080
mode = "thumb"
name = "a_dest"
dispatch = false
image = "img_a"
[[extra_func]]
addr = 0x03001060
mode = "thumb"
name = "b_hop"
dispatch = false
image = "img_b"
[[extra_func]]
addr = 0x03001080
mode = "thumb"
name = "b_dest"
dispatch = false
image = "img_b"
[[extra_func]]
addr = 0x03001000
mode = "thumb"
name = "b_root"
dispatch = false
image = "img_b"
[[data_range]]
start = 0x03001020
end = 0x03001024
note = "B literal word"
image = "img_b"
''')
out = root / 'generated'
subprocess.run((str(compiler), '--rom', str(rom_path), '--config', str(config),
                '--out', str(out), '--max-functions', '50'), check=True,
               stdout=subprocess.DEVNULL)
header = (out / 'recompiled.h').read_text()
table = (out / 'dispatch_table.cpp').read_text()
public_table, private_table = table.split('kPrivateDispatchTable[]', 1)
body = ''.join(path.read_text() for path in sorted(out.glob('recompiled_*.cpp')))
for name in ('gf_pub_ram', 'gf_pub_q', 'gf_a_root', 'gf_a_f2', 'gf_a_f3', 'gf_b_root',
             'gf_a_hop', 'gf_a_dest', 'gf_b_hop', 'gf_b_dest'):
    assert name + '(void)' in header, name
# Public table holds exactly the public 0x03001000 body, never an overlay body.
assert 'gf_pub_ram' in public_table and 'gf_a_root' not in public_table
assert 'gf_b_root' not in public_table
# B's data word is never emitted as code; A's code at the same address is.
assert body.count('gf_a_f2') >= 1
assert '0x03001020u' in private_table
# Per-image visited scope: the same loop head 0x03001004 exists once per image,
# with distinct image-qualified symbols, and only in the private table.
for sym in ('gf_tfunc_img_a_03001004', 'gf_tfunc_img_b_03001004'):
    assert sym + '(void)' in header, sym
assert 'tfunc_03001004' not in header  # no unqualified overlay auto name
import re
sig = re.findall(r'void (gf_\w+)\(void\);\s+/\* 0x03001000 thumb', header)
assert sorted(sig) == ['gf_a_root', 'gf_b_root', 'gf_pub_ram'], sig
# Source lookup: the same guest PC is emitted from different backing bytes.
def fn_body(name):
    m = re.search(r'void %s\(void\) \{(.*?)\n\}\n' % name, body, re.S)
    assert m, name
    return m.group(1)
assert '0x00000002u' in fn_body('gf_a_root') and '0x00000003u' in fn_body('gf_b_root')
assert '0x00000001u' in fn_body('gf_pub_ram')
# B's literal word (0x03001020) is data: no function is generated at that PC
# for image B, while image A generates code there.
assert not re.search(r'\{0x03001020u, 1u, \d+u, 2u,', private_table)
assert re.search(r'\{0x03001020u, 1u, 0u, 1u, gf_a_f2\}', private_table)
# Public table contains no overlay entry at all (image column only exists in
# the private table).
assert 'uint16_t image' in table
# A's call into the public world goes to the public body by name.
assert 'runtime_dispatch(_blt_' in fn_body('gf_a_f3')  # public call, by address
# An in-image indirect transfer (bx r7) is a TAIL transfer, never a nested dispatch.
assert 'runtime_dispatch_with_exchange_tail(' in fn_body('gf_a_hop')
assert 'runtime_dispatch_with_exchange(' not in fn_body('gf_a_hop')
print('fixture generated')

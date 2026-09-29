#!/usr/bin/env python3
"""Config-level fail-closed cases for image-scoped CFGs (synthetic)."""
import pathlib
import subprocess
import sys

compiler, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
cfg = (root / 'fixture.toml').read_text()
rom = root / 'fixture.gba'


def run(text, tag):
    path = root / f'neg_{tag}.toml'
    path.write_text(text)
    return subprocess.run(
        (str(compiler), '--rom', str(rom), '--config', str(path), '--out',
         str(root / f'neg_{tag}_out'), '--max-functions', '50'),
        capture_output=True, text=True)


def expect_fail(text, tag, needle):
    r = run(text, tag)
    out = r.stdout + r.stderr
    assert r.returncode != 0, f'{tag}: expected failure'
    assert needle in out, f'{tag}: missing "{needle}" in:\n{out[-800:]}'


assert run(cfg, 'ok').returncode == 0, 'baseline fixture config must generate'
# Unknown image id on an extra_func.
expect_fail(cfg.replace('image = "img_b"\nname', 'image = "nope"\nname')
            if 'image = "img_b"\nname' in cfg else
            cfg.replace('name = "b_root"\ndispatch = false\nimage = "img_b"',
                        'name = "b_root"\ndispatch = false\nimage = "nope"'),
            'unknown_extra', "not a declared [[executable_image]]")
# Unknown image id on a data_range.
expect_fail(cfg.replace('note = "B literal word"\nimage = "img_b"',
                        'note = "B literal word"\nimage = "nope"'),
            'unknown_range', "not a declared [[executable_image]]")
# The primary image is not a valid scope.
expect_fail(cfg.replace('name = "b_root"\ndispatch = false\nimage = "img_b"',
                        'name = "b_root"\ndispatch = false\nimage = "primary"'),
            'primary', "primary image")
# image requires dispatch = false.
expect_fail(cfg.replace('name = "b_root"\ndispatch = false\nimage = "img_b"',
                        'name = "b_root"\nimage = "img_b"'),
            'dispatch', "requires dispatch = false")
# Overlap is only legal between two overlays.
expect_fail(cfg.replace('sha256 = "', 'sha256 = "', 1).replace(
    'overlay = true\n', '', 1), 'overlap_plain', "overlap")
# An image-scoped range does not contradict a function of another scope, but a
# global range at an image function does.
expect_fail(cfg + '''
[[data_range]]
start = 0x03001000
end = 0x03001002
note = "global range over an overlay entry"
''', 'global_range', "falls inside")
print('image_scoped_cfg_negative_tests: PASS')

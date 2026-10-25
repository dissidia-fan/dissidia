#!/usr/bin/env python3
"""Self-test of the pure helpers of new_character.py (no game data needed): python3 -I test_new_character.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
if __name__ == "__main__":
    import pydeps                    # numpy / Pillow / scipy at the pinned versions (tools/pydeps.py)
    pydeps.require()
from PIL import Image
import gim_encode as ge            # noqa: E402
import new_character as nc         # noqa: E402
from PIL import Image, ImageDraw   # noqa: E402

assert nc.split_series("FINAL FANTASY IX") == ("FINAL FANTASY", "IX")
assert nc.split_series("FINAL FANTASY 7") == ("FINAL FANTASY", "7")
assert nc.split_series("KINGDOM HEARTS") == ("KINGDOM HEARTS", "")
assert nc.split_series("Chrono Trigger") == ("Chrono Trigger", "")

assert nc.check_slot(32, 43)[0] is False
assert nc.check_slot(43, 43)[0] is False
assert nc.check_slot(33, 43)[0] and nc.check_slot(34, 43)[0]
ok, notes = nc.check_slot(35, 43)
assert ok and any("extra charId" in n for n in notes)
assert not any("EXPERIMENTAL" in n for n in nc.check_slot(34, 43)[1])

lines = ["# roster", "34 noc100 9 tile=1", ""]
assert nc.roster_find(lines, 34) == "noc100" and nc.roster_find(lines, 35) is None
l2 = nc.roster_edit(lines, 35, "35 viv100 27")
assert l2 == ["# roster", "34 noc100 9 tile=1", "35 viv100 27"], l2
assert nc.roster_edit(l2, 35, "35 viv100 27 name=2")[-1] == "35 viv100 27 name=2"
assert nc.roster_edit(l2, 34, None) == ["# roster", "35 viv100 27"]
assert nc.roster_edit(["34 a"], 34, None) == []

im = Image.new("RGBA", (400, 400), (0, 0, 0, 0))                     # a head and shoulders picture with transparent margins
ImageDraw.Draw(im).rectangle((100, 50, 299, 349), fill=(200, 100, 50, 255))
x0, y0, x1, y1 = ge.cover_crop(im, 32 / 46, 0.2)
assert 100 <= x0 and x1 <= 300 and 50 <= y0 and y1 <= 350, (x0, y0, x1, y1)
assert abs((x1 - x0) / (y1 - y0) - 32 / 46) < 0.02
assert ge.cover_crop(im, 4.0, 0.3)[2] - ge.cover_crop(im, 4.0, 0.3)[0] == 200
# HUD portrait (chara_image): lives in the retail box, lower part clipped by the retail ellipse, rest alpha 0
h = ge.frame_hud(im, (128, 128))
al = h.getchannel("A")
bx0, by0, bx1, by1 = ge.HUD_BOX
assert al.getbbox()[0] >= bx0 - 1 and al.getbbox()[2] <= bx1 + 1 and al.getbbox()[1] >= by0 - 1 and al.getbbox()[3] <= by1, al.getbbox()
assert al.getpixel((66, 99)) > 0 and al.getpixel((bx0 + 1, 98)) == 0 and al.getpixel((bx1 - 2, 98)) == 0     # ellipse corners cut
assert al.getpixel((2, 2)) == 0 and h.getpixel((2, 2))[:3] == (255, 255, 255)
opaque = Image.new("RGBA", (300, 300), (30, 60, 200, 255))                # a picture with a background gets the full oval
assert ge.frame_hud(opaque).getchannel("A").getpixel((bx0 + 1, by0 + 1)) == 0
assert ge.prepare(opaque, (128, 128), "hud").size == (128, 128)

# knockout_white: white ground removed (also an enclosed gap), figure kept, other pictures untouched
im = Image.new("RGBA", (200, 200), (255, 255, 255, 255))
for x in range(60, 140):
    for y in range(40, 160):
        im.putpixel((x, y), (30, 40, 90, 255))
for x in range(90, 110):                      # an enclosed white gap inside a second block
    for y in range(100, 140):
        im.putpixel((x, y), (255, 255, 255, 255))
for x in range(60, 90):
    for y in range(100, 140):
        im.putpixel((x, y), (30, 40, 90, 255))
ko = ge.knockout_white(im)
assert ko.getpixel((5, 5))[3] == 0 and ko.getpixel((100, 60))[3] == 255
assert ko.getpixel((100, 120))[3] == 0
tr = Image.new("RGBA", (10, 10), (255, 255, 255, 0))
assert ge.knockout_white(tr) is tr
print("ok")


"""Label unmapped private-use code points of a TrueType font.

Usage: font_probe.py <standard.ttf> <base.ttf> <letter> <out.ttf>

Every code point in U+E000-U+F8FF that <base.ttf> does not map gets a glyph that reads <letter>
followed by its four hex digits, built from the digits and letters of <standard.ttf>. Serve the
result with EDEN_NX2_BUILTIN_FONT_<type> to see which code point a title draws as "?" and in
which font. Needs fontTools.
"""
import sys
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen

std_path, base_path, tag, out_path = sys.argv[1:5]
std = TTFont(std_path)
font = TTFont(base_path)
std_cmap = std.getBestCmap()
cmap = font.getBestCmap()
upem_scale = font['head'].unitsPerEm / std['head'].unitsPerEm
order = font.getGlyphOrder()
glyf = font['glyf']
hmtx = font['hmtx']
std_gs = std.getGlyphSet()

chars = '0123456789ABCDEF' + tag
parts = {}
for ch in chars:
    name = std_cmap[ord(ch)]
    if base_path == std_path:
        parts[ch] = (name, std['hmtx'][name][0])
        continue
    pen = TTGlyphPen(None)
    std_gs[name].draw(TransformPen(pen, (upem_scale, 0, 0, upem_scale, 0, 0)))
    new = 'probe_part_' + ch
    glyf[new] = pen.glyph()
    adv = round(std['hmtx'][name][0] * upem_scale)
    hmtx[new] = (adv, glyf[new].xMin if hasattr(glyf[new], 'xMin') else 0)
    parts[ch] = (new, adv)

scale = 0.5
added = 0
for cp in range(0xE000, 0xF900):
    if cp in cmap:
        continue
    label = tag + '%04X' % cp
    pen = TTGlyphPen(glyf)
    x = 0
    for ch in label:
        name, adv = parts[ch]
        pen.addComponent(name, (scale, 0, 0, scale, x, 0))
        x += adv * scale
    gname = 'probe_%04X' % cp
    glyf[gname] = pen.glyph()
    hmtx[gname] = (round(x), 0)
    for sub in font['cmap'].tables:
        if sub.isUnicode() and sub.format in (4, 12):
            sub.cmap[cp] = gname
    added += 1
font.setGlyphOrder(list(glyf.glyphOrder))
font['post'].formatType = 3.0
font.save(out_path)
check = TTFont(out_path)
print(out_path, 'added', added, 'glyphs', check['maxp'].numGlyphs, 'cmap', len(check.getBestCmap()))

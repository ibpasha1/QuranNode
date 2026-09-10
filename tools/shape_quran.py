#!/usr/bin/env python3
"""shape_quran.py — offline Arabic mushaf shaper -> QuranNode glyph pack.

This is the whole "hard part" of Arabic on the device, done once on the desktop:
HarfBuzz shapes each ayah's Uthmani text (RTL, contextual joining, tashkeel) and
FreeType rasterizes the shaped glyphs into an 8-bit alpha coverage bitmap. We also
record a bounding box per source word (in reading order) so the device can
highlight the word being recited. Everything is packed into one ".qgp" file per
font size; the device just blits (see core/arabic/arabic_text.c).

Pack format (little-endian) — must match core/arabic/arabic_text.h:
  header { magic "QNGP", u16 version, u16 flags, u16 line_h, u16 ascent,
           u32 n_entries, u32 index_off }                         (20 bytes)
  index[n]{ u16 surah, u16 ayah, u32 blob_off, u16 w, u16 h, u16 n_words, u16 pad }
  blob    { u8 alpha[w*h], box[n_words]{ u16 x,y,w,h } }

M0 embeds a small sample (Al-Fatiha). M1's build_data.py will feed the full
bundled sample set (Al-Mulk, Al-Kahf 1-20) through shape_ayah() the same way.
"""
import argparse
import os
import struct
import uharfbuzz as hb
import freetype

FONT_DEFAULT = os.path.join(os.path.dirname(__file__), "fonts", "AmiriQuran-Regular.ttf")

# --- M0 sample: Surah Al-Fatiha, Uthmani text ---------------------------------
AL_FATIHA = [
    (1, 1, "بِسْمِ ٱللَّهِ ٱلرَّحْمَٰنِ ٱلرَّحِيمِ"),
    (1, 2, "ٱلْحَمْدُ لِلَّهِ رَبِّ ٱلْعَٰلَمِينَ"),
    (1, 3, "ٱلرَّحْمَٰنِ ٱلرَّحِيمِ"),
    (1, 4, "مَٰلِكِ يَوْمِ ٱلدِّينِ"),
    (1, 5, "إِيَّاكَ نَعْبُدُ وَإِيَّاكَ نَسْتَعِينُ"),
    (1, 6, "ٱهْدِنَا ٱلصِّرَٰطَ ٱلْمُسْتَقِيمَ"),
    (1, 7, "صِرَٰطَ ٱلَّذِينَ أَنْعَمْتَ عَلَيْهِمْ غَيْرِ ٱلْمَغْضُوبِ عَلَيْهِمْ وَلَا ٱلضَّآلِّينَ"),
]

# --- Tajweed rule colors ------------------------------------------------------
# Rule names come from cpfair/quran-tajweed (riwayat Hafs). We group them into a
# small, legible palette that mirrors the Dar al-Marifah colour Mushaf; the DEVICE
# palette (scene_reader.c TAJWEED_PAL) MUST use the same index->RGB mapping.
# Index 0 = default (uncolored) text.
#   1 red       = obligatory madd (madd laazim 6, madd waajib muttasil)
#   2 amber     = permissible madd (2/4/6 harakaat, munfasil)
#   3 green     = ghunnah family — every rule pronounced WITH a nasal ghunnah
#                 (ghunnah, ikhfa, iqlab, idghaam WITH ghunnah, idghaam shafawi)
#   4 blue      = qalqalah (echoed letters)
#   5 grey      = silent / merged, pronounced WITHOUT ghunnah (hamzat-wasl,
#                 sun-letter laam, silent letters, idghaam without ghunnah)
#   6 dark blue = tafkhim (heavy raa, and the heavy laam of the name Allah).
#                 Not in the cpfair dataset — computed from the text below.
TAFKHIM = 6
RULE_COLOR = {
    "madd_6": 1, "madd_muttasil": 1,
    "madd_2": 2, "madd_246": 2, "madd_munfasil": 2,
    # green: nasalised rules (ghunnah audible)
    "ghunnah": 3, "idghaam_ghunnah": 3, "idghaam_shafawi": 3,
    "ikhfa": 3, "ikhfa_shafawi": 3, "iqlab": 3,
    "qalqalah": 4,
    # grey: silent / merged with no ghunnah (note: rule names match the JSON
    # exactly — the *_mutajanisayn / *_mutaqaribayn spellings were wrong before,
    # so those annotations silently fell through to uncolored).
    "hamzat_wasl": 5, "silent": 5, "lam_shamsiyyah": 5,
    "idghaam_no_ghunnah": 5, "idghaam_mutajanisayn": 5, "idghaam_mutaqaribayn": 5,
}
# Preview-only RGB (device has the matching palette). Keep in sync with the device.
PREVIEW_PALETTE = {
    0: (210, 218, 230), 1: (255, 90, 90), 2: (255, 170, 70),
    3: (70, 210, 130), 4: (95, 170, 255), 5: (120, 120, 135),
    6: (70, 120, 235),
}

TAJWEED_DIR = os.path.join(os.path.dirname(__file__), "quran-tajweed")

# Text + annotation sources. Overridden by --text-file / --annot-file so the
# waqf-marked variant (tools/build_waqf_text.py) can be shaped without touching
# the plain build. They must be a matched pair: the annotation offsets index
# into the text codepoints.
TEXT_PATH = os.path.join(TAJWEED_DIR, "quran-uthmani.txt")
ANNOT_PATH = os.path.join(TAJWEED_DIR, "output", "tajweed.hafs.uthmani-pause-sajdah.json")


def load_tanzil_surah(surah):
    """Return {ayah: text} for `surah` from the Tanzil Uthmani text the tajweed
    annotations were built against (so codepoint offsets line up)."""
    path = TEXT_PATH
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or "|" not in line:
                continue
            s, a, text = line.split("|", 2)
            if int(s) == surah:
                out[int(a)] = text
    return out


# ---- Tafkhim (heavy pronunciation) --------------------------------------
# The cpfair dataset has no tafkhim rule, so we derive the two cases the colour
# Mushaf marks in dark blue directly from the Uthmani text: the laam of the name
# Allah when it is heavy, and heavy raa (per the classical raa rules). Genuinely
# ambiguous / jawaaz raa cases are left uncolored rather than risk a wrong colour.
_FATHA  = {0x064E, 0x064B}          # fatha, fathatan
_DAMMA  = {0x064F, 0x064C}          # damma, dammatan
_KASRA  = {0x0650, 0x064D}          # kasra, kasratan
_SUKOON = 0x0652
_SHADDA = 0x0651
_DAGGER = 0x0670                    # superscript alef (a long -aa-, fatha-based)
_RA, _LAM, _HEH = 0x0631, 0x0644, 0x0647
_ALEF, _ALEF_WASLA, _YEH = 0x0627, 0x0671, 0x064A
_ISTILA = set("خصضغطقظ")            # huroof al-isti'la (inherently heavy letters)


def _is_mark(ch):
    o = ord(ch)
    return (0x064B <= o <= 0x065F) or o == _DAGGER or (0x06D6 <= o <= 0x06ED) \
        or (0x0610 <= o <= 0x061A) or o == 0x0640


def _marks_after(text, i):
    """(set of combining-mark codepoints on base char i, index just past cluster)."""
    s, j = set(), i + 1
    while j < len(text) and _is_mark(text[j]):
        s.add(ord(text[j])); j += 1
    return s, j


def _space_between(text, i, j):
    return " " in text[i + 1:j]


def _jalalah_is_heavy(text, bases, k):
    """Heavy unless the last real vowel before the shadda'd laam is a kasra."""
    j = k - 1
    while j >= 0:
        b = bases[j]; c = ord(text[b]); mk, _ = _marks_after(text, b)
        if c in (_ALEF, _ALEF_WASLA):        # connecting alef, no vowel of its own
            j -= 1; continue
        if c == _LAM and not (mk & (_FATHA | _DAMMA | _KASRA)):
            j -= 1; continue                 # silent article laam
        if mk & _KASRA:  return False
        if mk & (_FATHA | _DAMMA):  return True
        j -= 1                               # saakin letter: keep looking back
    return True                              # utterance start -> heavy


def _ra_is_heavy(text, bases, k, i, marks):
    if marks & (_FATHA | _DAMMA) or _DAGGER in marks:  return True   # raa + fatha/damma
    if marks & _KASRA:  return False                                 # raa + kasra
    # Saakin raa: explicit sukoon, or a bare final letter (pausal form).
    is_final = (k + 1 >= len(bases)) or _space_between(text, i, bases[k + 1])
    if not (_SUKOON in marks or (is_final and not marks)):
        return False                         # unclear medial state -> leave default
    if k == 0:  return True
    pb = bases[k - 1]; pc = ord(text[pb]); pm, _ = _marks_after(text, pb)
    if pc == _ALEF_WASLA:  return True        # kasra 'aaridah (connecting hamza)
    if pc == _YEH and not (pm & (_FATHA | _DAMMA | _KASRA)):  return False  # yaa saakina
    if pm & (_FATHA | _DAMMA):  return True
    if pm & _KASRA:
        # kasra before saakin raa: heavy only if a non-kasra isti'la follows in-word
        if k + 1 < len(bases):
            nb = bases[k + 1]
            if not _space_between(text, i, nb) and text[nb] in _ISTILA:
                nm, _ = _marks_after(text, nb)
                if not (nm & _KASRA):  return True
        return False
    if k >= 2:                                # preceding letter saakin -> look further
        ppm, _ = _marks_after(text, bases[k - 2])
        return bool(ppm & (_FATHA | _DAMMA))
    return True


def tafkhim_indices(text):
    """{codepoint index -> TAFKHIM} for heavy raa and the heavy laam of Allah."""
    out = {}
    bases = [i for i, ch in enumerate(text) if ch != " " and not _is_mark(ch)]
    for k, i in enumerate(bases):
        ch = ord(text[i])
        marks, nxt = _marks_after(text, i)
        if ch == _RA:
            if _ra_is_heavy(text, bases, k, i, marks):
                for m in range(i, nxt):       # the raa + its own marks
                    out[m] = TAFKHIM
        elif ch == _LAM and _SHADDA in marks and k >= 1 and k + 1 < len(bases) \
                and ord(text[bases[k - 1]]) == _LAM \
                and ord(text[bases[k + 1]]) == _HEH:
            # doubled laam + heh == the name Allah (not e.g. "lahu", one laam).
            if _jalalah_is_heavy(text, bases, k):
                for m in range(bases[k - 1], nxt):   # the "لله" ligature + shadda
                    out[m] = TAFKHIM
    return out


def load_color_maps(surah, ayah_texts):
    """Return {ayah: [palette_index per codepoint]} from the tajweed JSON,
    overlaid with computed tafkhim colours where the JSON leaves text uncolored."""
    import json
    path = ANNOT_PATH
    data = json.load(open(path))
    maps = {}
    for e in data:
        if e["surah"] != surah:
            continue
        ayah = e["ayah"]
        text = ayah_texts.get(ayah, "")
        n = len(text)
        arr = [0] * n
        for ann in e["annotations"]:
            idx = RULE_COLOR.get(ann["rule"], 0)
            if idx == 0:
                continue
            for cp in range(ann["start"], min(ann["end"], n)):
                arr[cp] = idx
        for cp, v in tafkhim_indices(text).items():
            if cp < n and arr[cp] == 0:       # never override an existing rule colour
                arr[cp] = v
        maps[ayah] = arr
    return maps


class Shaper:
    def __init__(self, font_path, px):
        with open(font_path, "rb") as fh:
            data = fh.read()
        self.hb_face = hb.Face(data)
        self.hb_font = hb.Font(self.hb_face)
        upem = self.hb_face.upem
        self.hb_font.scale = (upem, upem)         # shape in font units, we scale below
        self.px = px
        self.scale = px / upem                    # font-units -> pixels
        self.ft = freetype.Face(font_path)
        self.ft.set_pixel_sizes(0, px)
        # Vertical metrics in pixels.
        self.ascent = int(self.ft.size.ascender >> 6)
        self.descent = int(-(self.ft.size.descender >> 6))
        self.line_h = self.ascent + self.descent

    def _word_index_map(self, text):
        """code-point index -> source word index (words split on ASCII space).

        HarfBuzz cluster values (for a Python str added via Buffer.add_str) are
        CODE-POINT indices, not UTF-8 byte offsets — so this map must be indexed
        by character position to line glyphs up with their source word.
        """
        m = {}
        word = 0
        for i, ch in enumerate(text):
            if ch == " ":
                word += 1
                m[i] = -1        # the space itself belongs to no word
            else:
                m[i] = word
        return m, word + 1

    def _line_width_px(self, text):
        """Shaped advance width of a single line, in pixels (for wrapping)."""
        buf = hb.Buffer()
        buf.add_str(text)
        buf.guess_segment_properties()
        hb.shape(self.hb_font, buf, {"kern": True, "liga": True})
        return sum(p.x_advance for p in buf.glyph_positions) * self.scale

    def _render_run(self, text, line_colors=None):
        """Rasterize ONE line of Arabic. Returns (alpha, colidx, w, h, boxes).
        colidx is a per-pixel palette index (or None when line_colors is None);
        boxes are per-word bboxes (reading order) relative to this line's bitmap.

        HarfBuzz with RTL direction returns glyphs already in visual order
        (left->right), so we lay them out with a positive advancing pen.
        """
        buf = hb.Buffer()
        buf.add_str(text)
        buf.guess_segment_properties()            # script=Arabic, direction=RTL, lang
        hb.shape(self.hb_font, buf, {"kern": True, "liga": True})
        infos = buf.glyph_infos
        positions = buf.glyph_positions

        byte2word, n_words = self._word_index_map(text)

        # End-of-ayah enclosure fix: the rosette (U+06DD) encloses the ayah-number
        # digits, which HarfBuzz emits as ZERO-advance glyphs *before* the rosette
        # base, with an x_offset measured from the pen AFTER the rosette advances.
        # Because the verse-end is the visual-leftmost thing, that leading run of
        # zero-advance glyphs sits at the very start of the stream (a normal line
        # starts with an advancing letter, so lead==0 and nothing changes). Shift
        # that run right by the base's advance so the digits land inside the circle.
        lead = 0
        while lead < len(positions) and positions[lead].x_advance == 0:
            lead += 1
        lead_shift = positions[lead].x_advance if 0 < lead < len(positions) else 0

        # First pass: place each glyph, collect owning word + tajweed color.
        placed = []   # (gid, pen_x_px, pen_y_px, word_idx, color_idx)
        pen = 0.0
        for gi, (info, pos) in enumerate(zip(infos, positions)):
            shift = lead_shift if gi < lead else 0
            gx = (pen + pos.x_offset + shift) * self.scale
            gy = (pos.y_offset) * self.scale
            col = 0
            if line_colors and 0 <= info.cluster < len(line_colors):
                col = line_colors[info.cluster]
            placed.append((info.codepoint, gx, gy, byte2word.get(info.cluster, -1), col))
            pen += pos.x_advance

        # Rasterize glyphs to find real ink bounds (FreeType bitmaps), in BOTH
        # axes. The horizontal bounds were always the real ink; the vertical
        # used to be the font's metric line height, which CLIPPED marks whose
        # ink rises above the ascender (shadda-stacks, superscript alif, high
        # sukun) — dropping vowels, worse at large sizes. Track y too.
        glyph_bmps = []
        min_x = 1e9
        max_x = -1e9
        min_y = 1e9
        max_y = -1e9
        for gid, gx, gy, _w, _col in placed:
            self.ft.load_glyph(gid, freetype.FT_LOAD_RENDER)
            bm = self.ft.glyph.bitmap
            left = self.ft.glyph.bitmap_left
            top = self.ft.glyph.bitmap_top
            ox = gx + left
            oy = self.ascent - top - gy
            glyph_bmps.append((ox, oy, bm.width, bm.rows, bytes(bm.buffer), bm.pitch))
            if bm.width and bm.rows:
                min_x = min(min_x, ox)
                max_x = max(max_x, ox + bm.width)
                min_y = min(min_y, oy)
                max_y = max(max_y, oy + bm.rows)

        if max_x < min_x:      # empty
            return b"", None, 0, 0, [(0, 0, 0, 0)] * n_words

        pad = 4
        origin_x = min_x - pad
        # Always cover at least the metric line box [0, line_h] so ordinary
        # lines are byte-for-byte identical to before; expand only when ink
        # overshoots, so previously-clipped marks now fit.
        top_y = min(min_y, 0.0)
        bot_y = max(max_y, float(self.line_h))
        origin_y = top_y - pad
        w = int(round(max_x - min_x)) + 2 * pad
        h = int(round(bot_y - top_y)) + 2 * pad
        alpha = bytearray(w * h)
        colidx = bytearray(w * h) if line_colors else None

        # Second pass: composite coverage (+ color) and per-word bboxes.
        word_bbox = {}
        for (ox, oy, gw, gh, buf_bytes, pitch), (_gid, _gx, _gy, word, col) in zip(glyph_bmps, placed):
            bx = int(round(ox - origin_x))
            by = int(round(oy - origin_y))
            for row in range(gh):
                dy = by + row
                if dy < 0 or dy >= h:
                    continue
                src = row * pitch
                for coln in range(gw):
                    v = buf_bytes[src + coln]
                    if v == 0:
                        continue
                    dx = bx + coln
                    if dx < 0 or dx >= w:
                        continue
                    idx = dy * w + dx
                    if v > alpha[idx]:
                        alpha[idx] = v
                        if colidx is not None:
                            colidx[idx] = col
            if word >= 0 and gw:
                bb = word_bbox.get(word)
                x0, y0, x1, y1 = bx, by, bx + gw, by + gh
                if bb is None:
                    word_bbox[word] = [x0, y0, x1, y1]
                else:
                    bb[0] = min(bb[0], x0); bb[1] = min(bb[1], y0)
                    bb[2] = max(bb[2], x1); bb[3] = max(bb[3], y1)

        boxes = []
        for wi in range(n_words):
            bb = word_bbox.get(wi)
            if bb is None:
                boxes.append((0, 0, 0, 0))
            else:
                x0 = max(0, bb[0]); y0 = max(0, bb[1])
                boxes.append((x0, y0, max(0, bb[2] - x0), max(0, bb[3] - y0)))
        return bytes(alpha), (bytes(colidx) if colidx is not None else None), w, h, boxes

    def shape_ayah(self, text, max_w=430, color_at=None):
        """Shape a full ayah, wrapping words to fit `max_w`. Returns
        (alpha, colidx_or_None, w, h, boxes). If color_at (a palette index per
        ayah codepoint) is given, a tajweed color plane is produced."""
        words = text.split(" ")

        # Codepoint start of each word within the ayah (for slicing color_at).
        word_cp, off = [], 0
        for wtok in words:
            word_cp.append(off)
            off += len(wtok) + 1

        # Greedy line packing by shaped width.
        lines = []          # (global_start_word, [word strings])
        cur, cur_start = [], 0
        for i, wtok in enumerate(words):
            trial = " ".join(cur + [wtok])
            if cur and self._line_width_px(trial) > max_w:
                lines.append((cur_start, cur))
                cur, cur_start = [wtok], i
            else:
                if not cur:
                    cur_start = i
                cur.append(wtok)
        if cur:
            lines.append((cur_start, cur))

        # Render each line, slicing the ayah color map to the line's codepoints.
        rendered = []
        for start, ws in lines:
            line_text = " ".join(ws)
            lc = None
            if color_at is not None:
                cp0 = word_cp[start]
                lc = color_at[cp0:cp0 + len(line_text)]
            rendered.append((start, self._render_run(line_text, lc)))

        line_gap = max(2, int(self.line_h * 0.12))
        total_w = max((r[1][2] for r in rendered), default=0)
        total_h = sum(r[1][3] for r in rendered) + line_gap * (len(rendered) - 1)
        alpha = bytearray(total_w * total_h)
        colidx = bytearray(total_w * total_h) if color_at is not None else None

        n_words = len(words)
        boxes = [(0, 0, 0, 0)] * n_words

        y_off = 0
        for start, (la, lci, lw, lh, lboxes) in rendered:
            x_off = (total_w - lw) // 2
            for ry in range(lh):
                dst = (y_off + ry) * total_w + x_off
                src = ry * lw
                for rx in range(lw):
                    v = la[src + rx]
                    if v:
                        alpha[dst + rx] = v
                        if colidx is not None and lci is not None:
                            colidx[dst + rx] = lci[src + rx]
            for li, (bx, by, bw, bh) in enumerate(lboxes):
                if bw and bh:
                    boxes[start + li] = (bx + x_off, by + y_off, bw, bh)
            y_off += lh + line_gap

        return (bytes(alpha), (bytes(colidx) if colidx is not None else None),
                total_w, total_h, boxes)


def build_pack(ayat, font_path, px, out_path, color_maps=None, preview_dir=None, maxw=300):
    sh = Shaper(font_path, px)
    has_color = color_maps is not None
    entries = []   # (surah, ayah, alpha, colidx, w, h, boxes)
    for surah, ayah, text in ayat:
        cmap = color_maps.get(ayah) if color_maps else None
        alpha, colidx, w, h, boxes = sh.shape_ayah(text, max_w=maxw, color_at=cmap)
        entries.append((surah, ayah, alpha, colidx, w, h, boxes))
        print(f"  {surah}:{ayah}  {w}x{h}  {len(boxes)} words{'  (tajweed)' if colidx else ''}")
        if preview_dir:
            _save_preview(preview_dir, surah, ayah, alpha, colidx, w, h, boxes)

    # Layout: header(20) + index(n*16) + blobs.
    # blob = alpha[w*h] (+ colidx[w*h] if colored) + boxes[n_words]*8
    n = len(entries)
    index_off = 20
    blob_off = index_off + n * 16
    index = bytearray()
    blobs = bytearray()
    for (surah, ayah, alpha, colidx, w, h, boxes) in entries:
        off = blob_off + len(blobs)
        index += struct.pack("<HHIHHHH", surah, ayah, off, w, h, len(boxes), 0)
        blobs += alpha
        if has_color:
            blobs += colidx if colidx else bytes(w * h)
        for (x, y, ww, hh) in boxes:
            blobs += struct.pack("<HHHH", x, y, ww, hh)

    flags = 1 if has_color else 0   # bit0 = per-pixel tajweed color plane present
    header = struct.pack("<4sHHHHII", b"QNGP", 1, flags, sh.line_h, sh.ascent, n, index_off)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(index)
        f.write(blobs)
    print(f"wrote {out_path}  ({len(header)+len(index)+len(blobs)} bytes, {n} ayat, "
          f"line_h={sh.line_h}, color={has_color})")


def _save_preview(preview_dir, surah, ayah, alpha, colidx, w, h, boxes):
    from PIL import Image, ImageDraw
    os.makedirs(preview_dir, exist_ok=True)
    img = Image.new("RGB", (w, h), (6, 7, 12))
    px = img.load()
    for y in range(h):
        for x in range(w):
            v = alpha[y * w + x]
            if not v:
                continue
            base = PREVIEW_PALETTE[colidx[y * w + x]] if colidx else (210, 218, 230)
            px[x, y] = (base[0] * v // 255, base[1] * v // 255, base[2] * v // 255)
    d = ImageDraw.Draw(img)
    for (x, y, ww, hh) in boxes:
        if ww and hh:
            d.rectangle([x, y, x + ww - 1, y + hh - 1], outline=(40, 90, 60))
    img.save(os.path.join(preview_dir, f"{surah}_{ayah}.png"))


def _parse_surahs(spec):
    """'1,78-114' -> [1, 78, 79, ... 114]."""
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-")
            out.extend(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    return out


def main():
    ap = argparse.ArgumentParser(description="Shape Quran ayat into QuranNode per-surah glyph packs")
    ap.add_argument("--font", default=FONT_DEFAULT)
    ap.add_argument("--px", type=int, default=40, help="glyph pixel size")
    ap.add_argument("--maxw", type=int, default=300, help="text column width px (portrait=300)")
    ap.add_argument("--surahs", default="1", help="which surahs, e.g. '1,78-114'")
    ap.add_argument("--outdir", default="sdcard/packs/reader_lg",
                    help="per-surah <n>.qgp files are written here")
    ap.add_argument("--tajweed", action="store_true", help="bake tajweed rule colors")
    ap.add_argument("--preview", metavar="DIR", help="also write per-ayah preview PNGs")
    ap.add_argument("--text-file", help="override Uthmani text (e.g. quran-uthmani-waqf.txt)")
    ap.add_argument("--annot-file", help="override tajweed annotations JSON (must match --text-file)")
    args = ap.parse_args()

    global TEXT_PATH, ANNOT_PATH
    if args.text_file:
        TEXT_PATH = args.text_file
    if args.annot_file:
        ANNOT_PATH = args.annot_file

    have_tanzil = os.path.exists(TEXT_PATH)
    os.makedirs(args.outdir, exist_ok=True)
    surahs = _parse_surahs(args.surahs)
    print(f"font={args.font} px={args.px} maxw={args.maxw} tajweed={args.tajweed} "
          f"surahs={surahs[0]}..{surahs[-1]} -> {args.outdir}")

    for surah in surahs:
        if have_tanzil:
            tanzil = load_tanzil_surah(surah)
            ayat = [(surah, a, tanzil[a]) for a in sorted(tanzil)]
        elif surah == 1:
            ayat = AL_FATIHA
            tanzil = {a: t for _s, a, t in AL_FATIHA}
        else:
            print(f"  skip surah {surah}: no Tanzil text")
            continue
        color_maps = load_color_maps(surah, tanzil) if args.tajweed else None
        out = os.path.join(args.outdir, f"{surah}.qgp")
        build_pack(ayat, args.font, args.px, out, color_maps, args.preview, args.maxw)


if __name__ == "__main__":
    main()

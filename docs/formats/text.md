# Fonts and localisation

* `fonts/<name>.png` + `fonts/<name>.fnt` (bitmap fonts): plain-text `.fnt`, one decimal number per line, `cols*rows` lines, row-major by glyph index (`code - firstCode`): the horizontal advance in atlas pixels (0 = use the cell width). Grid by type: 0 = 8x8 from 0x20, 1 (numbers) = 4x4 from '0', 2 = 16x16 from 0. Text is upper-cased before lookup.
* `baked/<name>.ufnt` + `baked/<name>.png` (unicode font, localised copies under `baked/localize_<lang>/`):
  ```
  line 1: float lineHeight   (40)
  line 2: float baseline     (22)
  line 3: int   N            glyph count
  N lines: "<unicode codepoint> <glyph index>"
  N lines: "<x0> <y0> <x1> <y1> <xoff> <yoff> <xadvance>"  (atlas px rect with exclusive end, glyph offsets, advance)
  ```
* `locale/strings.txt` (English) and `locale/localize_<lang>/strings.txt` (de es fr it ja ko ru zh): `key=value` per line (UTF-8); keys keep only letters, `_`, `-`; `{name}` placeholders replaced by `mgSetUText(id, key, name, value, ...)`.


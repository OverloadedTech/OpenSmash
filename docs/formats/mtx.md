# `.mtx`: texture container

Header (12 bytes, LE): `u32 hasAlpha`, `u32 payloadSize`, `u32 0`. File length = 12 + payloadSize.

* `hasAlpha == 0` (15 files, mostly in `hud/`): payload is a plain JPEG.
* `hasAlpha == 1` (all other textures): payload =
  `u32 1, u32 width, u32 height, u32 jpegLen, JPEG[jpegLen], u32 alphaLen, zlib(alpha)[alphaLen]`
  where the zlib stream inflates to `width*height` bytes (8-bit alpha plane).
  RGBA = JPEG RGB + alpha plane.
Plain `.png` files (not `.mtx`) are used in `menu/`, `hud/`, `fonts/` and `baked/`; the `gfx/` sprite sheets are `.mtx`.

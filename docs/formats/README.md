# Asset formats

All paths are relative to the extracted asset root (see `tools/extract_assets.py`;
in the APK most files carry an extra `.mp3` suffix that the extractor strips).
Text files use CR or CRLF or LF line endings interchangeably and have no XML prolog.

| Doc | Files |
|---|---|
| [mesh.md](mesh.md) | `segments/**/*.mesh` baked static geometry |
| [mtx.md](mtx.md) | `*.png.mtx` and `*.jpg.mtx` textures (JPEG + optional alpha) |
| [content.md](content.md) | `game.xml`, `levels/*.xml`, `rooms/**/*.lua`, `segments/**/*.xml`, `obstacles/*.lua`, `materials.xml`, `templates.xml`, `meshes/*.xml` |
| [text.md](text.md) | `fonts/`, `baked/*.ufnt`, `locale/` |
| [shaders.md](shaders.md) | `shaders/*.glsl` |

`tools/validate_assets.py` checks an extracted folder against these descriptions, and `sh_asset_test` does the same
through the engine's own loaders.

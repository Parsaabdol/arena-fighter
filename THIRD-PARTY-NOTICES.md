# Third-party notices

Arena Fighter itself is MIT licensed — see [LICENSE](LICENSE). It bundles the
following third-party components, each of which carries its own terms.

## raylib

- **Version:** 6.0 (Windows x64, MSVC build)
- **Location in this repo:** `vendor/raylib-6.0_win64_msvc16/`
- **Home page:** https://www.raylib.com
- **License:** zlib/libpng — see `vendor/raylib-6.0_win64_msvc16/LICENSE`

The headers and prebuilt binaries are committed so that a fresh clone builds
with no setup step. The zlib licence permits redistribution in both source and
binary form; it asks only that the origin not be misrepresented and that the
notice not be removed, which this file and the bundled `LICENSE` satisfy.

## cgltf

- **Version:** 1.15
- **Location in this repo:** `vendor/cgltf/cgltf.h`
- **Home page:** https://github.com/jkuhlmann/cgltf
- **License:** MIT — see the notice at the end of `vendor/cgltf/cgltf.h`

Header only: the game declares against it and links the implementation that
raylib already compiles into `raylib.lib`. The header is deliberately pinned
to the same cgltf version raylib 6.0 bundles — if the raylib in `vendor/`
ever moves, this header moves with it.

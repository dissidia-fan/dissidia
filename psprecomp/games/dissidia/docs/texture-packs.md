# Texture packs

PPSSPP-style HD texture replacement: where the pack goes, how to dump and upscale, and the built-in upscaler.

## How it works

PPSSPP-style texture replacement, implemented in the generic runtime (`runtime/src/psp_tex_pack.cpp`,
hooked into `ge_texture_bind`); this game module only supplies the directory. Pack directory:
`PSPRECOMP_TEXPACK` (`run.sh --texpack DIR`), else `<mods>/textures` (`<mods>` as in "Modding: loose files").
Keep the pack outside `<mods>` when it is large: the pack is served by texture hash and is never a package
entry, and the loader ignores `<mods>/textures/` for the package index, but a separate folder keeps the mods
tree small. Dumps go to `PSPRECOMP_TEX_DUMP_DIR` (`run.sh --tex-dump DIR`, which also turns dumping on),
else `<pack>/dump`.

```
<mods>/textures/dump/<id>.png   dumped originals (PSPRECOMP_TEX_DUMP=1)
<mods>/textures/<id>.png        replacements, any size (2x/4x/8x ...), RGBA/RGB/gray/palette PNG, not interlaced
<mods>/textures/textures.txt    index: id WxH format clut-format first-seen-address times-seen
```

**Texture id.** 64-bit xxHash of the decoded source texels (unswizzled, packed to width x height, so it does
not depend on guest address, swizzle or row stride), the RGBA values of the palette entries the texels
actually reference (CLUT4/CLUT8 only: each palette variant of an image is its own id, unused palette
entries do not matter), the GE format and the size. It is computed only on a texture-cache miss and only
when dump, a non-empty pack or the upscaler is on, so the default cost is zero. Formats decoded by the
runtime (5650/5551/4444/8888/CLUT4/CLUT8) take part; DXT is not used by this game.

**Workflow.**
1. `PSPRECOMP_TEX_DUMP=1 ./run.sh ...`, play through the menus and a battle. Each distinct texture is written once
   (`PSPRECOMP_TEX_DUMP_MAX`, default 50000, caps the count; existing PNGs are never overwritten). Title + menus + one
   Quick Battle is about 300 distinct textures / 2.8 MB. About two thirds of them are 512x32 CLUT4 strips: those are
   the runtime-rendered text lines, they change with the text; skip them with `--min-size 64`.
2. Upscale: `python3 games/dissidia/tools/texpack.py <mods>/textures/dump --min-size 64 --cmd "realesrgan-ncnn-vulkan -i {in} -o {out} -s 4"`
   (`--list` prints the index with done/todo status, `--filter ">=32x32"`, `--format CLUT8`, `--limit`, `--dry-run`,
   `--scale-check 4`; finished ids are skipped, so it resumes). Any CLI works: Real-ESRGAN ncnn-vulkan
   (`-n realesrgan-x4plus-anime` for the UI art), `waifu2x-ncnn-vulkan -n 1 -s 2`, or ImageMagick
   `convert {in} -filter Lanczos -resize 400% {out}`. The tool installs and downloads nothing. If an upscaler
   drops the alpha channel it is restored from the original when Pillow is available.
3. Run normally: replacement is on whenever the directory has `<id>.png` files (`PSPRECOMP_TEX_REPLACE=0` turns it off).
   The file list is read once at start (no per-bind stat); a replacement is decoded on the cache miss and uploaded
   at its own size. UVs are normalised, so TEXSCALE/TEXOFFSET, REPEAT/CLAMP and both draw paths
   (batched GPU-skinning path, the default on every platform, and `PSPRECOMP_GE_FAST=0` for the legacy renderer) work unchanged; render scale is independent.
   A replacement more than 2x the original is uploaded with generated mip levels, used when the game's
   TEXFILTER asks for mipmapped minification. The game's filter is kept; `PSPRECOMP_TEX_REPLACE_FILTER=linear|nearest`
   forces it on replaced textures. Render-target textures and movie frames never take part.

**Built-in upscaler (off by default).** `PSPRECOMP_TEX_UPSCALE=xbrz2|xbrz3|xbrz4` upscales textures that have no
replacement at decode time (results cached per id in memory, 192 MB budget; 4x = 2x applied twice). It is a
clean-room edge-directed pixel scaler (Scale2x/Scale3x rules with a colour-distance equality test, alpha
aware), not the GPL xBRZ code; the names only select the factor. It helps hard-edged UI art, not photographic
textures.

Notes: ids of textures with partly uninitialised guest memory can differ between runs; textures drawn at
runtime (text strips) produce a new id per distinct content. Stats are logged as `[TEXPACK]` lines.

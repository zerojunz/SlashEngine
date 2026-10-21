# Texture forms

Exports ship textures in the forms each platform's GPUs sample, as Godot imports a texture once
for each platform. [main.cpp](main.cpp) makes a project in code with a sprite, a small spark and
a UI icon, and a `project.json` that compresses textures except those in `ui/`:

```json
"rendering": {"textures": {"compress_mode": "vram_compressed", "paths": {"ui/": "lossless"}}}
```

1. `texture.forms` for a desktop, a phone and the web makes `sprites/hero.png` in BC7 (macOS,
   Windows, Linux) and ASTC 4x4 (Android, iOS), both of which the web ships, since browsers have
   one or the other. Each form takes a byte a texel on the GPU, a quarter of RGBA8, and its
   `psnr` says how close its colors stay: about 40 dB here. The spark is 6x6, which 4x4 blocks
   don't fit, so it ships as it is, and the note says why; the icon is `lossless`. Each
   platform's totals add up what it ships and what the textures take on its GPU.
2. The next call reads the forms from the asset library's cache: `"made": 0`, `"cached": true`.
   Exports read the same cache, so a texture is compressed once for every export and project.
3. A web export ships `sprites/hero.png.bc7.ktx2` and `sprites/hero.png.astc_4x4.ktx2` in place
   of `sprites/hero.png`, and scenes keep naming `sprites/hero.png`. A game reads the form its
   GPU samples (`canvas::with_texture_forms`): BC7 on a desktop GPU, ASTC on a phone's. A GPU
   that samples neither reads one anyway and decodes it when it loads.

- Forms are KTX2 files, one mip level of BC7 or ASTC 4x4 blocks or RGBA8 texels;
  `vram_uncompressed` ships RGBA8, as big on disk as on the GPU but ready to upload.
- The cache is `forms/` in the asset library (`SLASH_LIBRARY_DIR`, else `~/.slash/library`),
  keyed by the texture file's SHA-256 and the format; here the session's `library_dir` puts it
  in `library/`.
- Compressing keeps colors close rather than exact, so pixel art and UI often stay `lossless`.

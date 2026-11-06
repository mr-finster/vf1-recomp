# Third-party code

- `mame/`: MAME's V60 core and disassembler, BSD-3-Clause (see mame/README.md for what else was ported from MAME).
- `musashi/`: Karl Stenerud's Musashi 68000 core, MIT (see musashi/readme.txt). The opcode tables are generated at build time by m68kmake.
- `ymfm/`: Aaron Giles' ymfm (YM3438 / OPN2), BSD-3-Clause (see ymfm/LICENSE), as in MAME's 3rdparty/ymfm.
- `rmlui/`: RmlUi 6.3 (MIT, see rmlui/LICENSE.txt), the in-game menu: Include, Source/Core and the SDL
  platform and renderer backends. Three changes: RmlUi_Renderer_SDL.cpp loads images with SDL 3.4's own
  SDL_LoadSurface_IO (BMP, PNG) instead of SDL_image, has BeginOverlay(): BeginFrame without the clear, and uses SDL3's built-in premultiplied blend
  (the same blend as its custom one, which the software renderer doesn't support).
  For the menus' languages: fallback faces ordered by language, then style and weight (`Rml::SetFallbackFontOrder`,
  FontProvider, FontFaceHandleDefault), and lines broken between Chinese, Japanese and Korean letters with the kinsoku
  rules (ElementText's BuildToken).
- `stb/`: Sean Barrett's stb_image 2.30 (public domain / MIT, see the end of the file), for loading the
  hi-res backgrounds (PNG, JPG, BMP).

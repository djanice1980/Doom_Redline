REDLINE high-resolution menu font
=================================

The 64 PNGs here are Doom's STCFN font glyphs (STCFN033 to STCFN121 from
doom.wad) upscaled four times with Real-ESRGAN (realesrgan-x4plus-anime, after
a 2x nearest-neighbour step, box-filtered from 8x to 4x). The game draws them
in place of the WAD's 8-pixel letters for any text at 3x or larger; small text
keeps the original pixels. Regenerate with tools/font_upscale.sh.

The letter shapes are id Software's, from the Doom font, so these files are a
derivative of the game data and are not covered by REDLINE's MIT licence. They
are only ever used when the player's own WAD contains the matching glyph
(manifest.txt records each source glyph's size and a checksum of its shape),
so without Doom they do nothing. Delete this folder and the game falls back to
its own xBR upscaling of the WAD font. Should the rights holder object, the
folder will be removed.

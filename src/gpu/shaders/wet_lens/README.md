# Wet-lens vertex shader

`B0B81D68886E069D.bin` is the retail container at XEX 0x82061320, created by `_effects_init` and bound only by `setWetSceneConstants` (0x82289970) for the water-drop quads `applyWetToScene` (0x82289D70) draws over the screen, e.g. when Deep Blue's water cannon hits the camera. Regenerate `wet_lens_shaders.h` with `python tools/shaders/build_wet_lens_shader.py`.

The guest draws these quads with `D3DRS_VIEWPORTENABLE` off, so the positions are window pixels and the shader samples `backBufferTexture` at `(x * c11.x, 1 - y * c11.y)` with c11 = (1/W, 1/H, 1, 0). `CaptureVertices` converts pixel positions to NDC before the shader runs, which leaves that UV pinned to one corner texel. The variant computes `(0.5x + 0.5, 0.5 - 0.5y)` from the NDC position instead. It assumes the guest viewport covers the whole backbuffer, which holds outside split screen.

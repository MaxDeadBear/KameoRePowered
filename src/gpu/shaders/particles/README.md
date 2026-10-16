# Dust-mote point sprites

The retail containers here are the exact shader pair from the missing basin particles: vertex `67EDA3D674722EC6`, pixel `9A16289A84E5A85B`. Regenerate the embedded variants with `python tools/shaders/build_particle_shaders.py`.

The September 10 basin capture contains five point-list draws using this pair: 40,000 ambient motes and four local groups of 1,000, 1,000, 5,000, and 1,000 points. Their vertex-buffer creation caller is `0x8235A530`, inside `pliteDustMoteCreateVertexBuffer`. IDA MCP confirms `_dustMote_coreRender` at `0x8235BD60` binds the pair, uploads motion/color/fog constants, and issues `D3DPT_POINTLIST`.

The original vertex instruction at physical code offset 552 is `C801803F 00000000 E2000000`: export 63.x, the particle diameter. The existing translator emits an unused `r63.x` assignment. The generated native variant forwards this value to a geometry shader. The pixel shader's r2 is the generated parameter register; its texture fetch uses r2.zw. The variant supplies screen position and sprite coordinates there instead of zero.

The geometry stage expands each point into four screen-facing vertices, retaining depth, clip-space W, colors, and fog. Size is clamped using the guest PA_SU_POINT_MINMAX register. IDA verifies the point-size store at device+0x2DA4 and `D3D::WriteLazyRenderState` at 0x820D54C0 flushes the register block to 0x2280; MINMAX is the next register, device+0x2DA8. The read-only SDK implementation confirms the fixed-point conversion is 0.125 pixels per register unit and the NDC radius is diameter divided by viewport extent.

The water-cannon spray uses the same scheme: vertex `511A3E3E3F0761D3` (XEX 0x8203D588) and pixel `E1D5926C5F6332C6` (XEX 0x8203D8B0), created by `WaterCannonJetRenderInit` and drawn as `D3DPT_POINTLIST` by `drawSpray` (0x824FFD48). Its vertex shader exports the size from r63.x too, and its pixel shader's parameter register is r1. Pairs are listed in `SPRITES` in the build script and `g_pointSprites` in `src/gpu/shaders/embedded_shaders.cpp`.

Only point-sprite draws with a listed shader pair use the variants. Other shaders and post-processing keep their existing paths. Both DXIL and SPIR-V are compiled ahead of time into particle_shaders.h. Runtime visual validation is required; compiling SPIR-V does not establish Vulkan runtime correctness.

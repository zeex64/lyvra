# Anime4K shaders

From https://github.com/bloc97/Anime4K (commit 7684e95, MIT, see LICENSE): the
`Anime4K_Upscale_CNN_x2_{S,M,L}.glsl` mpv user shaders, unmodified.

Lyvra converts each mpv pass into a Vulkan compute shader at build time
(`tools/gen_anime4k.py`) and runs the passes itself.

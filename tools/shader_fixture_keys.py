#!/usr/bin/env python3
"""Write synthetic pixel shader keys (struct nv2a_pixel_shader_key, xgpu.h, as the
32-bit guest lays it out) that reach the translator's paths no recorded scene does:
every texture stage mode, the fog modes, every alpha test, alpha kill and color
sign. debug.gpu_shader_replay translates them like recorded keys, so a replay of
this folder checks that every path's output compiles (tools/msl_compile.sh).

shader_fixture_keys.py FOLDER
"""

import struct
import sys
from pathlib import Path

PS_MAX = 57                 # D3DRS_PS_MAX: the combiner state words
COMBINER_COUNT = 53         # D3DRS_PSCOMBINERCOUNT, an index into them
KEY_SIZE = 252
SAMPLER_NONE, SAMPLER_2D, SAMPLER_3D, SAMPLER_CUBE = 0, 1, 2, 3
D3DCMP_NEVER = 0x200        # the Xbox's D3DCMP_* run NEVER (0x200) to ALWAYS (0x207)

# texture stage modes (nv2a_psh.c) and the sampler type each samples
MODES = {
    0x01: SAMPLER_2D,    # project2d
    0x02: SAMPLER_3D,    # project3d
    0x03: SAMPLER_CUBE,  # cubemap
    0x04: SAMPLER_NONE,  # passthru
    0x05: SAMPLER_NONE,  # clipplane
    0x06: SAMPLER_2D,    # bumpenvmap
    0x07: SAMPLER_2D,    # bumpenvmap_luminance
    0x08: SAMPLER_2D,    # brdf
    0x09: SAMPLER_2D,    # dot_st
    0x0a: SAMPLER_NONE,  # dot_zw
    0x0b: SAMPLER_CUBE,  # dot_reflect_diffuse
    0x0c: SAMPLER_CUBE,  # dot_reflect_specular
    0x0d: SAMPLER_3D,    # dot_str_3d
    0x0e: SAMPLER_CUBE,  # dot_str_cube
    0x0f: SAMPLER_2D,    # dependent_ar
    0x10: SAMPLER_2D,    # dependent_gb
    0x11: SAMPLER_NONE,  # dot_product
    0x12: SAMPLER_CUBE,  # dot_reflect_specular_constant
}


def key(modes, samplers, alpha_kill=(0, 0, 0, 0), color_sign=(0, 0, 0, 0), alpha_test=0, fog=0, fog_mode=0):
    """the bytes of one key: one combiner stage, the given texture stages"""
    state = [0] * PS_MAX
    state[COMBINER_COUNT] = 1
    texture_modes = sum(mode << (5 * stage) for stage, mode in enumerate(modes))
    data = struct.pack(f"<{PS_MAX}II4B4B4BIBBBB", *state, texture_modes, *samplers, *alpha_kill, *color_sign,
                       alpha_test, fog, fog_mode, 0, 0)
    assert len(data) == KEY_SIZE
    return data


def keys():
    """name -> bytes"""
    result = {}
    # each mode at stage 3, behind the stages the dot product modes read
    for mode, sampler in MODES.items():
        result[f"ps_mode{mode:02x}"] = key((0x01, 0x11, 0x11, mode), (SAMPLER_2D, SAMPLER_NONE, SAMPLER_NONE, sampler))
        result[f"ps_mode{mode:02x}_stage1"] = key((0x01, mode, 0, 0), (SAMPLER_2D, sampler, 0, 0))
    for fog_mode in range(4):
        result[f"ps_fog{fog_mode}"] = key((0x01, 0, 0, 0), (SAMPLER_2D, 0, 0, 0), fog=1, fog_mode=fog_mode)
    for function in range(D3DCMP_NEVER, D3DCMP_NEVER + 8):
        result[f"ps_alpha{function - D3DCMP_NEVER}"] = key((0x01, 0, 0, 0), (SAMPLER_2D, 0, 0, 0), alpha_test=function)
    result["ps_kill_sign"] = key((0x01, 0x01, 0, 0), (SAMPLER_2D, SAMPLER_2D, 0, 0), alpha_kill=(1, 1, 0, 0),
                                 color_sign=(0x0f, 0x05, 0, 0))
    return result


def main():
    folder = Path(sys.argv[1])
    folder.mkdir(parents=True, exist_ok=True)
    for name, data in keys().items():
        (folder / f"{name}.key").write_bytes(data)


if __name__ == "__main__":
    main()

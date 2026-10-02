"""tools/shader_fixture_keys.py: synthetic pixel shader keys"""
import struct

from tools import shader_fixture_keys


def test_every_key_has_the_guest_layout_and_one_combiner_stage():
    keys = shader_fixture_keys.keys()
    assert all(len(data) == shader_fixture_keys.KEY_SIZE for data in keys.values())
    assert all(struct.unpack_from("<I", data, 4 * shader_fixture_keys.COMBINER_COUNT)[0] == 1 for data in keys.values())


def test_every_texture_mode_reaches_stage_three():
    keys = shader_fixture_keys.keys()
    for mode in shader_fixture_keys.MODES:
        (texture_modes,) = struct.unpack_from("<I", keys[f"ps_mode{mode:02x}"], 4 * shader_fixture_keys.PS_MAX)
        assert (texture_modes >> 15) & 0x1f == mode


def test_alpha_tests_use_the_xbox_compare_values():
    keys = shader_fixture_keys.keys()
    (never,) = struct.unpack_from("<I", keys["ps_alpha0"], 244)
    (always,) = struct.unpack_from("<I", keys["ps_alpha7"], 244)
    assert (never, always) == (0x200, 0x207)

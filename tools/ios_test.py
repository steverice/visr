#!/usr/bin/env python3
"""Run native ILP32, memory-tracking and SDL audio handoff regressions."""
import re
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/ios/probe'
BUILD.mkdir(parents=True, exist_ok=True)

def run(*args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)

clang = '/opt/homebrew/opt/llvm/bin/clang'
lld = '/opt/homebrew/opt/lld/bin/ld.lld'
run(clang, '--target=arm64_32-apple-watchos', '-mcpu=cortex-a53', '-ffixed-x15', '-ffixed-x27',
    '-mllvm', '-aarch64-enable-compress-jump-tables=false', '-fno-stack-protector', '-ffreestanding',
    '-O2', '-S', 'port/ios/tests/guest_probe.c', '-o', BUILD/'probe.darwin.s')
run('python3', 'tools/ios_asm_convert.py', BUILD/'probe.darwin.s', BUILD/'probe.s')
run(clang, '--target=aarch64-none-elf', '-c', BUILD/'probe.s', '-o', BUILD/'probe.o')
run(lld, '-m', 'aarch64linux', '-static', '-nostdlib', '-T', 'port/ios/guest.ld', BUILD/'probe.o', '-o', BUILD/'probe.elf')
run('python3', 'tools/ios_embed_guest.py', BUILD/'probe.elf', BUILD/'embed')
run('xcrun','clang','-O2',f'-I{BUILD}/embed','port/ios/tests/probe_runner.c',
    'port/ios/host/guest_call.S',BUILD/'embed/guest_image.S','-o',BUILD/'runner')
run(BUILD/'runner')
# gpu.h crosses the guest/host boundary (step 4): the host's layout of its structs, as
# _Static_asserts, must hold under the guest's arm64_32 flags too
run('xcrun', 'clang', '-O2', '-Iport/linux/src', 'port/ios/tests/gpu_layout_probe.c', '-o', BUILD / 'gpu-layout-probe')
with open(BUILD / 'gpu_layout_check.h', 'w') as layout_check:
    subprocess.run([str(BUILD / 'gpu-layout-probe')], cwd=ROOT, check=True, stdout=layout_check)
run(clang, '--target=arm64_32-apple-watchos', '-mcpu=cortex-a53', '-ffixed-x15', '-ffixed-x27',
    '-fno-stack-protector', '-ffreestanding', '-Iport/linux/src', '-fsyntax-only', '-x', 'c', BUILD / 'gpu_layout_check.h')
# memory_probe.c includes guest_host.h, which includes gpu.h (port/linux/src)
run('xcrun','clang','-O2','-Iport/ios/host','-Iport/runtime/include','-Iport/linux/src',
    '-Iport/runtime/guest/runtime','port/ios/tests/memory_probe.c','port/ios/host/host_memory.c','-o',BUILD/'memory-probe')
run(BUILD/'memory-probe')
sdl_flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl3'], text=True))
run('xcrun', 'clang', '-O2', '-DHALO_IOS=1', '-Iport/ios/host', '-Iport/ios/host',
    '-Iport/runtime/include', 'port/ios/tests/audio_probe.c', '-Wl,-dead_strip',
    *sdl_flags, '-o', BUILD/'audio-probe')
run(BUILD/'audio-probe')

run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/display_probe.c', '-o', BUILD/'display-probe')
run(BUILD/'display-probe')

# the native Mac runner's pinned display (host_display_pin.h)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/ios/host',
    'port/ios/tests/display_pin_probe.c', '-o', BUILD/'display-pin-probe')
run(BUILD/'display-pin-probe')
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/virtual_clock_probe.c', '-o', BUILD/'virtual-clock-probe')
run(BUILD/'virtual-clock-probe')
# halo://join links the app is opened with (host_join_link.h)
run('xcrun', 'clang', '-O2', '-Wall', '-Werror', '-fsanitize=address,undefined', '-Iport/ios/host',
    'port/ios/tests/join_link_probe.c', '-o', BUILD/'join-link-probe')
run(BUILD/'join-link-probe')
# the Metal backend's redundant-state filter (metal_state_cache.h)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/ios/host',
    'port/ios/tests/metal_state_probe.c', '-o', BUILD/'metal-state-probe')
run(BUILD/'metal-state-probe')

# debug.texture_override_directory's hash and file checks (port/linux/src/texture_override.h)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/texture_override_probe.c', '-o', BUILD/'texture-override-probe')
run(BUILD/'texture-override-probe')
# the texture upscale policy's device classifier (port/runtime/texture_policy.c)
run('python3', 'tools/embed_texture_policy.py', 'port/assets/texture-policy.json', BUILD/'texture_policy_table.c')
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/runtime', '-Iport/linux/src',
    'port/ios/tests/texture_policy_probe.c', 'port/runtime/texture_policy.c', BUILD/'texture_policy_table.c',
    '-lz', '-o', BUILD/'texture-policy-probe')
run(BUILD/'texture-policy-probe')
# the texture upscale cache's recipe key, invalidation, writes and storage rules (port/runtime/texture_cache.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/runtime', '-Iport/linux/src',
    'port/ios/tests/texture_cache_probe.c', 'port/runtime/texture_cache.c', '-o', BUILD/'texture-cache-probe')
run(BUILD/'texture-cache-probe')
# the per-level upscaled-texture gate (port/linux/src/texture_upscale_state.h)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/texture_upscale_state_probe.c', '-o', BUILD/'texture-upscale-state-probe')
run(BUILD/'texture-upscale-state-probe')
# the host's write of one setting into config.toml, for the Settings app's switch (port/ios/host/host_config.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/ios/host', 'port/ios/tests/host_config_write_probe.c',
    'port/ios/host/host_config.c', '-o', BUILD/'host-config-write-probe')
run(BUILD/'host-config-write-probe')
# the game's reread of one setting at a level load, the environment still winning (port/linux/src/port_config.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-DHALO_ILP32', '-Iport/linux/src',
    '-Iport/third_party/tomlc17', *shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'sdl3'], text=True)),
    'port/ios/tests/config_reload_probe.c', 'port/third_party/tomlc17/tomlc17.c', '-o', BUILD/'config-reload-probe')
run(BUILD/'config-reload-probe')
# bp and bpf on the GPU (port/ios/host/texture_refine.m); skips itself where there is no Metal device
run('xcrun', 'clang', '-O2', '-fobjc-arc', '-Iport/ios/host', 'port/ios/tests/texture_refine_probe.m',
    'port/ios/host/texture_refine.m', '-framework', 'Foundation', '-framework', 'Metal', '-o', BUILD/'texture-refine-probe')
run(BUILD/'texture-refine-probe')
# head-tracked stereo's look: the world holds still in the room while the head pans, and
# the right stick's turn (the probe includes port/linux/game/stereo.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-DHALO_IOS=1', '-Iport/linux/src', '-Iport/ios/host',
    'port/ios/tests/stereo_head_probe.c', 'port/ios/host/host_stereo_head.c',
    '-o', BUILD/'stereo-head-probe')
run(BUILD/'stereo-head-probe')
# SCREEN mode's 3D TV and the film: the one mapping, its ease, the reasons and the framing, and
# the first-person weapon's own eye (the probe includes port/linux/game/stereo.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-DHALO_IOS=1', '-Iport/linux/src', '-Iport/ios/host',
    'port/ios/tests/stereo_screen_probe.c', 'port/ios/host/host_stereo_head.c',
    '-o', BUILD/'stereo-screen-probe')
run(BUILD/'stereo-screen-probe')
# the head-tracked presenter's shaders, compiled from its source string at run time: compile
# them here, as the visionOS build's preprocessor leaves them (host_stereo_vignette.h's mask is
# macro text, which a math macro could otherwise rewrite unseen)
run('xcrun', '--sdk', 'xros', 'clang', '-target', 'arm64-apple-xros26.0', '-E', '-P', '-x', 'objective-c',
    '-Iport/ios/host', '-Iport/linux/src', '-Iport/runtime/include', '-Iport/runtime/guest/runtime',
    'port/ios/host/host_stereo.m', '-o', BUILD / 'host_stereo.i')
preprocessed = (BUILD / 'host_stereo.i').read_text()
literals = re.match(r'shader_source =\s*((?:@?"(?:[^"\\]|\\.)*"\s*)+);',
                    preprocessed[preprocessed.index('shader_source ='):])
(BUILD / 'host_stereo.metal').write_text(''.join(
    re.findall(r'"((?:[^"\\]|\\.)*)"', literals.group(1))).encode().decode('unicode_escape'))
run('xcrun', '--sdk', 'xros', 'metal', '-c', BUILD / 'host_stereo.metal', '-o', BUILD / 'host_stereo.air')
# HEAD mode turns in snaps unless the player asks otherwise
if '{ "input.turn", _config_string, "\\"snap\\""' not in (ROOT / 'port/linux/src/port_config.c').read_text():
    raise SystemExit('port_config.c: input.turn must default to "snap"')

# Parse untrusted XISO metadata and exercise extraction/cancellation under sanitizers.
run('python3', 'tools/ios_xiso_test.py')

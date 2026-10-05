#!/usr/bin/env python3
"""Run native ILP32, memory-tracking and SDL audio handoff regressions."""
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
# the Metal backend's redundant-state filter (metal_state_cache.h)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/ios/host',
    'port/ios/tests/metal_state_probe.c', '-o', BUILD/'metal-state-probe')
run(BUILD/'metal-state-probe')

# Parse untrusted XISO metadata and exercise extraction/cancellation under sanitizers.
run('python3', 'tools/ios_xiso_test.py')

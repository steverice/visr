#!/usr/bin/env python3
"""Run the iPad build on this Mac as a "Designed for iPad" app, and compare runs.

macOS launches an iPad app only if Xcode installed it. It also kills the app
that port/ios's CMake project builds ("Code Signature Invalid"), although the
same executable runs when a plain Xcode project signs it. So `run` generates a
small wrapper project around the CMake-built HaloCE executable and Info.plist,
installs and launches it through Xcode with the debugger off (memory_watch.c
write-protects pages and expects their faults, which would stop a debugger),
waits for the game to quit (debug.exit_after), and copies the logs,
screenshots and shader files out of the app's container. `compare` checks two
such result folders against each other.

`run --simulator UDID` runs a simulator build (an iOS or visionOS simulator app,
tools/ios_build.py --simulator) in that simulator instead, with the same
config.toml, init.txt and result folders, so its runs compare with the Mac's.
"""

import argparse
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

# a debug.gpu_stats summary (d3d8_device.c), after platform_log's prefix
STATS = re.compile(r"frame \d+: (.*)$")
# the GL call total at the end of a debug.gpu_stats summary
GL_CALLS = re.compile(r",? \d+ GL calls$")
# what a backend reports it can do (gpu_gl.c, gpu_metal.m), the same in every backend
CAPABILITIES = re.compile(r"GPU capabilities: (.*)$")
# a translated shader's source (.glsl or .metal) and the inputs it was translated from
SHADER_SOURCES = ("*.glsl", "*.metal")
SHADER_INPUTS = ("*.vsh", "*.key")


def merge_config(text, settings):
    """config.toml text with each dotted key in settings set to its raw TOML value"""
    lines = text.splitlines()
    for dotted, value in settings.items():
        section, key = dotted.split(".", 1)
        header = f"[{section}]"
        if header not in lines:
            lines += [header, f"{key} = {value}"]
            continue
        start = lines.index(header)
        end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("[")), len(lines))
        pattern = re.compile(rf"^{re.escape(key)}\s*=")
        for index in range(start + 1, end):
            if pattern.match(lines[index]):
                lines[index] = f"{key} = {value}"
                break
        else:
            insert = end
            while insert > start + 1 and not lines[insert - 1].strip():
                insert -= 1
            lines.insert(insert, f"{key} = {value}")
    return "\n".join(lines) + "\n"


# what port_config.c defaults these to: a run that doesn't --set one gets
# the default, not whatever an earlier run's --set left in config.toml
DEFAULTS = {
    "display.screen_width": "0",
    "display.render_height": "0",
    "display.vsync": "true",
    "display.renderer": '"gl"',
    "display.interpolation": "true",
    "display.anisotropic_filtering": "1",
    "display.shadow_map_size": "128",
    "display.effect_resolution": "false",
    "display.compressed_textures": "false",
    "display.frame_pacing": '"off"',
    "display.render_scale": "1.0",
    "display.upscaler": '"bilinear"',
    "display.high_res_hud": "true",
    "display.high_res_text": "true",
    "display.immersive": "false",
    "display.mirror_resolution": '"half"',
    "display.theater_width": "60.0",
    "display.theater_distance": "4.0",
    "display.theater_environment": '"passthrough"',
    "input.stick_dead_zone": "0.27",
    "debug.null_renderer": "false",
    "debug.gl_debug": "false",
    "debug.metal_state_cache": "true",
    "debug.fixed_timestep": "false",
    "debug.input_record": '""',
    "debug.input_replay": '""',
    "debug.benchmark": "false",
    "debug.gpu_stats": "false",
    "debug.gpu_trace_frame": "-1",
    "debug.gpu_trace_constants": "false",
    "debug.gpu_skip_vertex_shaders": '""',
    "debug.gpu_shader_replay_dialect": '""',
    "debug.gpu_debug_expression": '""',
    "debug.gpu_debug_texture0": "false",
    "debug.gpu_debug_flat": "false",
    "debug.texture_dump_directory": '""',
    "debug.texture_log": "false",
    "debug.texture_no_cache": "false",
}


def reset_settings(documents, screenshot_every, dump_shaders, replay):
    """the settings of a run before its --set ones: requested outputs go into
    runner/, the rest are off, and everything else is at its default"""
    runner = Path(documents) / "runner"
    return {
        **DEFAULTS,
        "debug.screenshot_every": str(screenshot_every),
        "debug.screenshot_directory": f'"{runner}/shots"' if screenshot_every else '""',
        "debug.gpu_dump_shaders": f'"{runner}/shaders"' if dump_shaders else '""',
        "debug.gpu_shader_replay": f'"{runner}/replay"' if replay else '""',
    }


def find_container(root, bundle_id):
    """the Data/Documents folder of the container macOS made for bundle_id, or None"""
    for metadata in sorted(Path(root).glob("*/.com.apple.containermanagerd.metadata.plist")):
        try:
            with metadata.open("rb") as file:
                identifier = plistlib.load(file).get("MCMMetadataIdentifier")
        except (OSError, plistlib.InvalidFileException):
            continue
        if identifier == bundle_id:
            return metadata.parent / "Data/Documents"
    return None


def read_bmp(data):
    """(width, height, BGRA rows) of a 32-bit BMP as write_screenshot (d3d8_device.c) writes it"""
    if len(data) < 30 or data[:2] != b"BM":
        raise ValueError("not a BMP")
    (offset,) = struct.unpack_from("<I", data, 10)
    width, height = struct.unpack_from("<ii", data, 18)
    (bits,) = struct.unpack_from("<H", data, 28)
    if bits != 32:
        raise ValueError(f"{bits}-bit BMP; expected 32")
    height = abs(height)
    if len(data) < offset + width * height * 4:
        raise ValueError(f"truncated: {len(data)} bytes")
    return width, height, data[offset:offset + width * height * 4]


def bmp_difference(a, b, channel_tolerance=0):
    """(pixels whose color differs by more than channel_tolerance in some channel,
    largest channel difference, (x, y) of the first pixel that difference is at, or
    None); alpha is ignored, since write_screenshot forces it opaque"""
    width_a, height_a, pixels_a = read_bmp(a)
    width_b, height_b, pixels_b = read_bmp(b)
    if (width_a, height_a) != (width_b, height_b):
        return max(width_a * height_a, width_b * height_b), 255, None
    if pixels_a == pixels_b:
        return 0, 0, None
    differing = largest = 0
    where = None
    row = width_a * 4
    for start in range(0, len(pixels_a), row):
        line_a, line_b = pixels_a[start:start + row], pixels_b[start:start + row]
        if line_a == line_b:
            continue
        for pixel in range(0, row, 4):
            delta = max(abs(line_a[pixel + channel] - line_b[pixel + channel]) for channel in range(3))
            if delta > channel_tolerance:
                differing += 1
            if delta > largest:
                largest, where = delta, (pixel // 4, start // row)
    return differing, largest, where


def bmp_pixels(data):
    """the pixel count of a BMP"""
    width, height, _ = read_bmp(data)
    return width * height


def stats_lines(log):
    """the debug.gpu_stats summaries in a log, without their frame numbers"""
    return [match.group(1) for match in map(STATS.search, log.splitlines()) if match]


def _files(folder, pattern):
    return {path.name: path for path in folder.glob(pattern)} if folder.is_dir() else {}


def _shader_files(folder, patterns):
    files = {}
    for pattern in patterns:
        files.update(_files(folder, pattern))
    return files


def _compare_files(problems, folder, a, b, files_a, files_b, contents):
    for name in sorted(files_a.keys() ^ files_b.keys()):
        problems.append(f"{folder}/{name} only in {a if name in files_a else b}")
    if contents:
        for name in sorted(files_a.keys() & files_b.keys()):
            if files_a[name].read_bytes() != files_b[name].read_bytes():
                problems.append(f"{folder}/{name} differs")


def capability_lines(log):
    """the GPU capability lines in a log"""
    return [match.group(1) for match in map(CAPABILITIES.search, log.splitlines()) if match]


def compare(a, b, tolerance=0, ignore_gl_calls=False, channel_tolerance=0, fraction=0.0, across_backends=False):
    """the differences between two result folders (empty: they match). A pixel
    differs when a channel differs by more than channel_tolerance, and a frame
    matches when no more than tolerance pixels, or fraction of its pixels,
    differ. With ignore_gl_calls the gpu_stats lines are compared without their
    GL call totals. across_backends compares a GL run with a Metal run: the
    shaders' inputs (.vsh, .key) must be the same files and their sources the
    same names, whatever their language; GL calls are ignored; and the two
    backends must report the same capabilities."""
    a, b = Path(a), Path(b)
    problems = []
    for folder in ("runner/shaders", "runner/replay/replay"):
        if across_backends:
            stems_a = {Path(name).stem: path for name, path in _shader_files(a / folder, SHADER_SOURCES).items()}
            stems_b = {Path(name).stem: path for name, path in _shader_files(b / folder, SHADER_SOURCES).items()}
            _compare_files(problems, folder, a, b, stems_a, stems_b, contents=False)
            _compare_files(problems, folder, a, b, _shader_files(a / folder, SHADER_INPUTS),
                           _shader_files(b / folder, SHADER_INPUTS), contents=True)
        else:
            _compare_files(problems, folder, a, b, _shader_files(a / folder, SHADER_SOURCES),
                           _shader_files(b / folder, SHADER_SOURCES), contents=True)
    shots_a, shots_b = _files(a / "runner/shots", "*.bmp"), _files(b / "runner/shots", "*.bmp")
    for name in sorted(shots_a.keys() ^ shots_b.keys()):
        problems.append(f"{name} only in {a if name in shots_a else b}")
    for name in sorted(shots_a.keys() & shots_b.keys()):
        try:
            data_a = shots_a[name].read_bytes()
            differing, largest, where = bmp_difference(data_a, shots_b[name].read_bytes(), channel_tolerance)
            allowed = max(tolerance, int(fraction * bmp_pixels(data_a)))
        except ValueError as error:
            problems.append(f"{name}: unreadable ({error})")
            continue
        if differing > allowed:
            at = f" at {where[0]},{where[1]}" if where else ""
            problems.append(f"{name}: {differing} pixels differ, by up to {largest}{at}")
    logs = [folder / "stderr.log" for folder in (a, b)]
    missing = [str(log) for log in logs if not log.is_file()]
    if missing:
        problems += [f"{log} missing" for log in missing]
    else:
        texts = [log.read_text(errors="replace") for log in logs]
        stats_a, stats_b = (stats_lines(text) for text in texts)
        if across_backends:
            capabilities_a, capabilities_b = (capability_lines(text) for text in texts)
            if not capabilities_a or capabilities_a != capabilities_b:
                problems.append(f"capabilities differ:\n  {a}: {capabilities_a}\n  {b}: {capabilities_b}")
        if ignore_gl_calls or across_backends:
            stats_a, stats_b = ([GL_CALLS.sub("", line) for line in lines] for lines in (stats_a, stats_b))
        if stats_a != stats_b:
            problems.append(f"gpu_stats differ:\n  {a}: {stats_a}\n  {b}: {stats_b}")
    return problems


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "build/mac-runner"
TARGET = "HaloRunner"
CONTAINERS = Path.home() / "Library/Containers"
DESTINATION = "platform=macOS,arch=arm64,variant=Designed for iPad"

# The run scheme turns off Xcode's GPU validation and frame capture. A scheme
# that doesn't say leaves Metal's API validation on, even with the debugger off:
# every Metal run got an MTLDebugDevice, whose checks cost most of a frame's CPU.
# --metal-validation turns validation on through the environment instead.
PROJECT = """name: {target}
options:
  bundleIdPrefix: org.haloce
targets:
  {target}:
    type: application
    platform: iOS
    deploymentTarget: "16.0"
    sources: [stub.c]
    settings:
      DEVELOPMENT_TEAM: {team}
      PRODUCT_BUNDLE_IDENTIFIER: {bundle_id}
      GENERATE_INFOPLIST_FILE: YES
      CURRENT_PROJECT_VERSION: "1"
      MARKETING_VERSION: "1.0"
      ENABLE_DEBUG_DYLIB: NO
    postBuildScripts:
      - name: Use the CMake-built HaloCE
        basedOnDependencyAnalysis: false
        script: |
          cp "{app}/HaloCE" "$TARGET_BUILD_DIR/$EXECUTABLE_PATH"
          cp "{app}/Info.plist" "$TARGET_BUILD_DIR/$INFOPLIST_PATH"
          plutil -replace CFBundleExecutable -string "$EXECUTABLE_NAME" "$TARGET_BUILD_DIR/$INFOPLIST_PATH"
          plutil -replace CFBundleIdentifier -string "$PRODUCT_BUNDLE_IDENTIFIER" "$TARGET_BUILD_DIR/$INFOPLIST_PATH"
schemes:
  {target}:
    build:
      targets:
        {target}: all
    run:
      config: Debug
      debugEnabled: false
      enableGPUValidationMode: disabled
      enableGPUFrameCaptureMode: disabled
{environment}"""

# Metal's API validation (gpu_metal.m), reported to the logs rather than
# stopping the app, so a run still reaches its screenshots
METAL_VALIDATION = {
    "MTL_DEBUG_LAYER": "1",
    "MTL_DEBUG_LAYER_ERROR_MODE": "nslog",
    "MTL_DEBUG_LAYER_WARNING_MODE": "nslog",
}
# shader validation on top: it checks every memory access the shaders make,
# which slows a10 past the run's time limit, and it reports the game's own
# NaN texture coordinates, which GL passes on silently
METAL_SHADER_VALIDATION = {
    "MTL_SHADER_VALIDATION": "1",
    "MTL_SHADER_VALIDATION_REPORT_TO_STDERR": "1",
}


def validation_environment(metal_validation, metal_shader_validation):
    """the scheme's environment for the validation a run asks for"""
    environment = {}
    if metal_validation or metal_shader_validation:
        environment.update(METAL_VALIDATION)
    if metal_shader_validation:
        environment.update(METAL_SHADER_VALIDATION)
    return environment


def scheme_environment(variables):
    """the run scheme's environmentVariables block for PROJECT ("" for none)"""
    if not variables:
        return ""
    return "      environmentVariables:\n" + "".join(f'        {name}: "{value}"\n' for name, value in variables.items())

# open -a returns before a cold Xcode has the project open, and a freshly
# loaded project lists its run destinations a little later still
# Xcode keeps one project of a name open: another checkout's runner project would
# block this one from opening, and be driven in its place. Match by path, not
# name: Xcode can list another runner project as ".../HaloRunner.xcodeproj/
# project.xcworkspace", named "project.xcworkspace", and one whose worktree was
# deleted stays open that way, leaving this project's run "not yet started" for
# good. Close each on its own, since such a stale one can fail to answer.
CLOSE_OTHERS = """tell application "{xcode}"
	repeat with other in (every workspace document whose path contains "/{target}.xcodeproj" and path does not start with "{project}")
		try
			close other saving no
		end try
	end repeat
end tell
"""

LAUNCH = """tell application "{xcode}"
	repeat 120 times
		if exists (first workspace document whose path is "{project}") then exit repeat
		delay 1
	end repeat
	set doc to first workspace document whose path is "{project}"
	repeat 120 times
		if loaded of doc then exit repeat
		delay 1
	end repeat
	repeat 60 times
		try
			set active run destination of doc to (first run destination of doc whose name is "My Mac (Designed for iPad)")
			exit repeat
		end try
		delay 1
	end repeat
	-- unguarded: a destination that never appeared fails here, not later
	set active run destination of doc to (first run destination of doc whose name is "My Mac (Designed for iPad)")
	-- right after opening or reloading the project Xcode sometimes answers "Cannot
	-- run", or is still loading it: wait for it, and try again
	set last_error to "Xcode did not report a run"
	set started_run to false
	repeat 3 times
		repeat 120 times
			if loaded of doc then exit repeat
			delay 1
		end repeat
		try
			set result_of_run to run doc
			repeat 120 times
				if (status of result_of_run as text) is not "not yet started" then exit repeat
				delay 1
			end repeat
			if (status of result_of_run as text) is not "error occurred" then
				set started_run to true
				exit repeat
			end if
			set last_error to (error message of result_of_run as text)
		on error message_of_error
			set last_error to message_of_error
		end try
		delay 5
	end repeat
	if not started_run then error "run failed after 3 attempts: " & last_error
end tell
"""


def run_command(*args, **options):
    print("+", " ".join(str(arg) for arg in args), flush=True)
    return subprocess.run([str(arg) for arg in args], check=True, **options)


def xcode_app():
    developer = subprocess.check_output(["xcode-select", "--print-path"], text=True).strip()
    return str(Path(developer).parents[1])


def running():
    return subprocess.run(["pgrep", "-x", TARGET], capture_output=True).returncode == 0


def started(documents, is_running):
    """whether the launched game is running or has run: a short run (debug.fixed_timestep
    makes one take seconds) can start and quit between two polls, leaving only its log"""
    return is_running() or (documents is not None and (documents / "ios-runtime.log").exists())


def wait_for(condition, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            return True
        time.sleep(2)
    return False


def build_wrapper(args):
    RUNNER.mkdir(parents=True, exist_ok=True)
    (RUNNER / "stub.c").write_text("int main(void) { return 0; }\n")
    (RUNNER / "project.yml").write_text(PROJECT.format(
        target=TARGET, team=args.team, bundle_id=args.bundle_id, app=args.app.resolve(),
        environment=scheme_environment(validation_environment(getattr(args, "metal_validation", False),
                                                             getattr(args, "metal_shader_validation", False)))))
    run_command("xcodegen", "generate", "--spec", RUNNER / "project.yml", "--project", RUNNER, "--quiet")
    run_command("xcodebuild", "-project", RUNNER / f"{TARGET}.xcodeproj", "-scheme", TARGET,
                "-destination", DESTINATION, "-allowProvisioningUpdates", "build",
                stdout=subprocess.DEVNULL)


def launch(documents=None):
    """install and start the wrapper through Xcode (the only way macOS accepts); documents
    is the container's Documents folder, once one exists"""
    xcode = xcode_app()
    project = RUNNER / f"{TARGET}.xcodeproj"
    run_command("osascript", input=CLOSE_OTHERS.format(xcode=xcode, target=TARGET, project=project), text=True)
    run_command("open", "-a", xcode, project)
    run_command("osascript", input=LAUNCH.format(xcode=xcode, target=TARGET, project=project), text=True)
    if not wait_for(lambda: started(documents, running), 300):
        sys.exit(f"{TARGET} did not start within 300 seconds; see Xcode's report navigator")


def container_documents(args):
    documents = find_container(CONTAINERS, args.bundle_id)
    if documents:
        return documents
    print("first run of this bundle id: launching once so macOS creates its container", flush=True)
    launch()
    if not wait_for(lambda: find_container(CONTAINERS, args.bundle_id), 120):
        sys.exit(f"no container appeared for {args.bundle_id}")
    subprocess.run(["pkill", "-x", TARGET])
    wait_for(lambda: not running(), 30)
    return find_container(CONTAINERS, args.bundle_id)


def prepare(args, documents):
    documents.mkdir(parents=True, exist_ok=True)
    if args.xiso and not (documents / "maps").is_dir():
        run_command("cp", "-c", args.xiso, documents / args.xiso.name)
    runner = documents / "runner"
    shutil.rmtree(runner, ignore_errors=True)
    runner.mkdir()
    # the game writes into these folders but does not create them
    if args.screenshot_every:
        (runner / "shots").mkdir()
    if args.dump_shaders:
        (runner / "shaders").mkdir()
    if args.replay:
        shutil.copytree(args.replay, runner / "replay",
                        ignore=lambda folder, names: [n for n in names if not n.endswith((".vsh", ".key"))])
    settings = reset_settings(documents, args.screenshot_every, args.dump_shaders, bool(args.replay))
    settings["debug.exit_after"] = f"{float(args.exit_after)}"
    for assignment in args.set:
        key, value = assignment.split("=", 1)
        settings[key.strip()] = value.strip()
    config = documents / "config.toml"
    config.write_text(merge_config(config.read_text() if config.is_file() else "", settings))
    init = documents / "init.txt"
    if args.init:
        init.write_text("\n".join(args.init) + "\n")
    elif init.exists():
        init.unlink()
    for name in ("stderr.log", "debug.txt", "ios-runtime.log"):
        (documents / name).unlink(missing_ok=True)
    (documents / "stderr.log").touch()


def collect(documents, out):
    out.mkdir(parents=True, exist_ok=True)
    for name in ("ios-runtime.log", "stderr.log", "debug.txt", "config.toml"):
        if (documents / name).is_file():
            shutil.copy2(documents / name, out / name)
    if (documents / "runner").is_dir():
        shutil.copytree(documents / "runner", out / "runner", dirs_exist_ok=True)


def simulator_pattern(udid):
    """a pgrep -f pattern matching the game's executable in simulator udid"""
    return f"CoreSimulator/Devices/{udid}/.*/HaloCE.app/HaloCE"


def simulator_running(udid):
    """the game is running in simulator udid"""
    return subprocess.run(["pgrep", "-f", simulator_pattern(udid)], capture_output=True).returncode == 0


def run_simulator(args):
    udid = args.simulator
    app = args.app or ROOT / "build/visionos/app-simulator/Release-xrsimulator/HaloCE.app"
    if not (app / "HaloCE").is_file():
        sys.exit(f"no simulator app at {app}; run tools/ios_build.py --simulator (--visionos) first")
    with (app / "Info.plist").open("rb") as file:
        bundle_id = plistlib.load(file)["CFBundleIdentifier"]
    # boots the simulator if it isn't, and waits until it has
    run_command("xcrun", "simctl", "bootstatus", udid, "-b", stdout=subprocess.DEVNULL)
    subprocess.run(["xcrun", "simctl", "terminate", udid, bundle_id], capture_output=True)
    run_command("xcrun", "simctl", "install", udid, app)
    container = subprocess.run(["xcrun", "simctl", "get_app_container", udid, bundle_id, "data"],
                               capture_output=True, text=True, check=True).stdout.strip()
    documents = Path(container) / "Documents"
    if args.maps and not (documents / "maps").is_dir():
        documents.mkdir(parents=True, exist_ok=True)
        run_command("cp", "-c", "-R", args.maps, documents / "maps")
    prepare(args, documents)
    run_command("xcrun", "simctl", "launch", udid, bundle_id, stdout=subprocess.DEVNULL)
    if not wait_for(lambda: simulator_running(udid), 60):
        collect(documents, args.out)
        sys.exit(f"the game did not start in simulator {udid} within 60 seconds; logs are in {args.out}")
    limit = args.time_limit or args.exit_after + 120
    finished = wait_for(lambda: not simulator_running(udid), limit)
    if not finished:
        subprocess.run(["xcrun", "simctl", "terminate", udid, bundle_id], capture_output=True)
    collect(documents, args.out)
    if not finished:
        sys.exit(f"the game was still running {limit} seconds after launch and was stopped; logs are in {args.out}")
    print(f"results: {args.out}")


def run(args):
    if args.simulator:
        run_simulator(args)
        return
    if not args.team:
        sys.exit("--team is required, except with --simulator")
    args.app = args.app or ROOT / "build/ios/app-device/Release-iphoneos/HaloCE.app"
    if not (args.app / "HaloCE").is_file():
        sys.exit(f"no CMake-built app at {args.app}; run tools/ios_build.py --team ... first")
    build_wrapper(args)
    documents = container_documents(args)
    prepare(args, documents)
    launch(documents)
    limit = args.time_limit or args.exit_after + 120
    finished = wait_for(lambda: not running(), limit)
    if not finished:
        subprocess.run(["pkill", "-x", TARGET])
    collect(documents, args.out)
    if not finished:
        sys.exit(f"{TARGET} was still running {limit} seconds after launch and was killed; "
                 f"logs are in {args.out}")
    print(f"results: {args.out}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    run_parser = commands.add_parser("run", help="run the game once and collect its results")
    run_parser.add_argument("--team", help="Apple development team ID (not needed with --simulator)")
    run_parser.add_argument("--bundle-id", default="org.haloce.macrunner")
    run_parser.add_argument("--app", type=Path,
                            help="the CMake-built device app (tools/ios_build.py --team ...; the default), or "
                                 "with --simulator the simulator app (default: the visionOS one)")
    run_parser.add_argument("--simulator", metavar="UDID",
                            help="run the simulator app in this simulator (xcrun simctl list devices)")
    run_parser.add_argument("--maps", type=Path,
                            help="with --simulator: a folder of imported maps to copy into a new container, "
                                 "in place of importing --xiso")
    run_parser.add_argument("--out", type=Path, required=True, help="folder to copy the results to")
    run_parser.add_argument("--xiso", type=Path, help="the player's XISO, imported on the first run")
    run_parser.add_argument("--exit-after", type=float, default=60.0, help="seconds before the game quits")
    run_parser.add_argument("--time-limit", type=float, default=0.0,
                            help="seconds after launch before the run is killed (default --exit-after + 120); "
                                 "with debug.fixed_timestep --exit-after counts frames, which validation slows")
    run_parser.add_argument("--set", action="append", default=[], metavar="SECTION.KEY=VALUE",
                            help="a config.toml setting, value in TOML syntax (repeatable)")
    run_parser.add_argument("--init", action="append", default=[], metavar="COMMAND",
                            help="a console command for init.txt, e.g. 'map_name a10' (repeatable)")
    run_parser.add_argument("--screenshot-every", type=int, default=0, metavar="FRAMES")
    run_parser.add_argument("--dump-shaders", action="store_true")
    run_parser.add_argument("--replay", type=Path, help="a folder of recorded .vsh/.key shader inputs")
    run_parser.add_argument("--metal-validation", action="store_true",
                            help="turn on Metal's API validation (for display.renderer=\"metal\" runs)")
    run_parser.add_argument("--metal-shader-validation", action="store_true",
                            help="turn on Metal's shader validation too (slow: a10 needs a longer --time-limit)")
    compare_parser = commands.add_parser("compare", help="compare two result folders")
    compare_parser.add_argument("a", type=Path)
    compare_parser.add_argument("b", type=Path)
    compare_parser.add_argument("--tolerance", type=int, default=0, help="pixels a frame may differ by")
    compare_parser.add_argument("--channel-tolerance", type=int, default=0, metavar="LEVELS",
                                help="channel difference a pixel may have and still match (0-255)")
    compare_parser.add_argument("--fraction", type=float, default=0.0,
                                help="fraction of a frame's pixels that may differ (overrides a smaller --tolerance)")
    compare_parser.add_argument("--ignore-gl-calls", action="store_true",
                                help="compare gpu_stats without the GL call totals")
    compare_parser.add_argument("--across-backends", action="store_true",
                                help="a GL run against a Metal run: shader inputs, not sources; capabilities; no GL calls")
    args = parser.parse_args()
    if args.command == "run":
        run(args)
    else:
        problems = compare(args.a, args.b, args.tolerance, args.ignore_gl_calls, args.channel_tolerance,
                           args.fraction, args.across_backends)
        print("\n".join(problems) if problems else "match")
        sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()

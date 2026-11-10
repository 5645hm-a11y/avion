"""Windows side of the head-to-head benchmark (docs/benchmarks.md). One JSON object per line on stdout.
- AVION's harness (MSVC Release) and ffmpeg's full-decode scene scan (software and D3D11VA) on the same files.
- Players on the same 720p H.264 clip, audio decoded but silent: the AVION demo, ffplay and VLC. CPU from GetProcessTimes
  (kernel + user) over a fixed window, playing, then paused; % of one logical core.
usage: python tools/bench/run_windows.py [--skip-players] > build/bench/windows_results.jsonl
Run with nothing else busy (not during the Linux side, a CTest suite or a video in a browser)."""
import ctypes, ctypes.wintypes as wt, json, pathlib, re, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parents[2]
# Paths relative to ROOT (the working directory of every run): the harness reads argv as UTF-8, and Windows passes a
# non-ASCII absolute path ("Développement") in the ANSI code page.
FIX = pathlib.Path("assets") / "test_fixtures"
FILES = [FIX / "costa_rica_4k60_hdr.mp4", FIX / "lg_black_4k60_hdr.mp4"]
RAW = pathlib.Path("build") / "bench" / "costa_640x480_gray.raw"
HARNESS = ROOT / "build" / "Release" / "media_bench_headtohead.exe"
DEMO = ROOT / "build" / "Release" / "media_player_demo.exe"
VLC = pathlib.Path(r"C:\Program Files\VideoLAN\VLC\vlc.exe")
PICTURES = 3000
k32, u32 = ctypes.windll.kernel32, ctypes.windll.user32


def emit(obj):
    print(json.dumps(obj), flush=True)


def ffmpeg_scan(path, pictures, threads, hw):
    args = ["ffmpeg", "-nostdin", "-hide_banner", "-benchmark"]
    if hw:
        args += ["-hwaccel", "d3d11va"]
    args += ["-threads", str(threads), "-i", str(path), "-frames:v", str(pictures), "-an",
             "-vf", "scdet", "-f", "null", "-"]
    run = subprocess.run(args, capture_output=True, text=True, errors="replace")
    bench = re.findall(r"utime=([\d.]+)s stime=([\d.]+)s rtime=([\d.]+)s", run.stderr)
    frames = re.findall(r"frame=\s*(\d+)", run.stderr)
    cuts = run.stderr.count("lavfi.scd.time")
    ut, st, rt = map(float, bench[-1]) if bench else (0, 0, 0)
    emit({"bench": "ffmpeg_scan", "platform": "windows", "file": path.name, "threads": threads, "decode": "d3d11va" if hw else "software",
          "pictures": int(frames[-1]) if frames else 0, "utime_s": ut, "stime_s": st, "rtime_s": rt, "cuts": cuts})


class FILETIME(ctypes.Structure):
    _fields_ = [("lo", wt.DWORD), ("hi", wt.DWORD)]


def cpu_seconds(handle):
    c, e, k, u = FILETIME(), FILETIME(), FILETIME(), FILETIME()
    k32.GetProcessTimes(handle, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u))
    return ((k.hi << 32 | k.lo) + (u.hi << 32 | u.lo)) / 1e7


def windows_of(pid):
    found = []
    proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def each(hwnd, _):
        owner = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and u32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    u32.EnumWindows(proc(each), 0)
    return found


def press_space(pid):
    # WM_KEYDOWN / WM_KEYUP for the space bar with its scan code (SDL reads the scan code from lParam).
    for hwnd in windows_of(pid):
        u32.PostMessageW(hwnd, 0x0100, 0x20, 0x00390001)
        u32.PostMessageW(hwnd, 0x0101, 0x20, 0xC0390001)


def measure(proc, seconds):
    handle = int(proc._handle)
    c0, t0 = cpu_seconds(handle), time.perf_counter()
    time.sleep(seconds)
    return 100.0 * (cpu_seconds(handle) - c0) / (time.perf_counter() - t0)


def player(name, args, pause_by_key=True, warm=5.0, playing=10.0, paused=20.0):
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(warm)
        play = measure(proc, playing) if playing else None
        if pause_by_key:
            press_space(proc.pid)
        time.sleep(2.0)
        idle = measure(proc, paused)
        emit({"bench": "player_cpu", "player": name, "playing_pct": round(play, 2) if play is not None else None, "paused_pct": round(idle, 2),
              "playing_s": playing, "paused_s": paused, "clip": FILES[0].name})
    finally:
        proc.kill()
        proc.wait()


def main():
    import os
    os.chdir(ROOT)
    cpu = subprocess.run(["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_Processor).Name"], capture_output=True, text=True).stdout.strip()
    ver = subprocess.run(["ffmpeg", "-hide_banner", "-version"], capture_output=True, text=True).stdout.split()[2]
    emit({"bench": "machine", "platform": "windows", "cpu": cpu, "ffmpeg": ver})
    for f in FILES:
        line = subprocess.run([str(HARNESS), "scan", str(f), str(PICTURES)], capture_output=True, text=True).stdout.strip()
        emit({"file": f.name, **json.loads(line)})
    for mode in ("flow", "survey"):
        emit(json.loads(subprocess.run([str(HARNESS), mode, str(RAW), "640", "480", "600"], capture_output=True, text=True).stdout))
    for f in FILES:
        ffmpeg_scan(f, PICTURES, 0, False)
        ffmpeg_scan(f, 600, 1, False)
        ffmpeg_scan(f, PICTURES, 0, True)
    if "--skip-players" in sys.argv:
        return
    clip = str(FILES[0])
    player("avion", [str(DEMO), clip, "--command", "mute"])
    # Playback alone: no warm perception grids (the default keeps perception live, so that T and Tab find known objects).
    player("avion_pure", [str(DEMO), clip, "--command", "mute", "--no-perception"])
    player("ffplay", ["ffplay", "-hide_banner", "-loglevel", "error", "-volume", "0", clip])
    player("vlc", [str(VLC), "--no-one-instance", "--gain", "0", "--no-video-title-show", clip])
    # VLC ignores a posted key: its paused figure comes from a second start, paused from the first picture.
    player("vlc_start_paused", [str(VLC), "--no-one-instance", "--gain", "0", "--no-video-title-show", "--start-paused", clip],
           pause_by_key=False, playing=0)


if __name__ == "__main__":
    main()

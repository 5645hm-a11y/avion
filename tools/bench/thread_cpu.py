"""Per-thread CPU of a running player (docs/benchmarks.md, "Where the player's CPU goes"): each thread's kernel + user time over
a window, playing, then paused (space posted to its window), as % of one logical core, with the thread's name
(SetThreadDescription: the engine names its own) or the module its start address is in (Media Foundation, the audio engine,
the GPU driver).
usage: python tools/bench/thread_cpu.py [--playing S] [--paused S] -- <player> <args...>"""
import ctypes, ctypes.wintypes as wt, json, pathlib, subprocess, sys, time

k32, u32, ntdll, psapi = ctypes.windll.kernel32, ctypes.windll.user32, ctypes.windll.ntdll, ctypes.windll.psapi
k32.OpenThread.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD), ("th32OwnerProcessID", wt.DWORD),
                ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long), ("dwFlags", wt.DWORD)]


def threads(pid):
    snap = k32.CreateToolhelp32Snapshot(4, 0)  # TH32CS_SNAPTHREAD
    entry = THREADENTRY32(); entry.dwSize = ctypes.sizeof(entry)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid: out.append(entry.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return out


def label(process, tid):
    h = k32.OpenThread(0x0800 | 0x0040, False, tid)  # THREAD_QUERY_LIMITED_INFORMATION | THREAD_QUERY_INFORMATION
    if not h: return "?"
    name = ctypes.c_wchar_p()
    try:
        if k32.GetThreadDescription(h, ctypes.byref(name)) >= 0 and name.value: return name.value
    finally:
        start = ctypes.c_void_p()
        ntdll.NtQueryInformationThread(h, 9, ctypes.byref(start), ctypes.sizeof(start), None)  # ThreadQuerySetWin32StartAddress
        k32.CloseHandle(h)
    buf = ctypes.create_unicode_buffer(260)
    if start.value and psapi.GetMappedFileNameW(process, ctypes.c_void_p(start.value), buf, 260):
        return buf.value.rsplit("\\", 1)[-1]
    return "?"


def times(tid):
    h = k32.OpenThread(0x0800, False, tid)
    if not h: return None
    c, e, k, u = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
    k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u))
    k32.CloseHandle(h)
    f = lambda t: (t.dwHighDateTime << 32 | t.dwLowDateTime) / 1e7
    return f(k) + f(u)


def sample(process, pid, seconds):
    before = {t: times(t) for t in threads(pid)}
    t0 = time.perf_counter(); time.sleep(seconds); wall = time.perf_counter() - t0
    rows = {}
    for t in threads(pid):
        now = times(t)
        if now is None: continue
        rows[t] = 100.0 * (now - (before.get(t) or 0.0)) / wall
    byname = {}
    for t, pct in rows.items():
        key = label(process, t)
        byname[key] = byname.get(key, 0.0) + pct
    return sum(rows.values()), dict(sorted(byname.items(), key=lambda kv: -kv[1]))


def main():
    args = sys.argv[1:]
    playing, paused = 10.0, 20.0
    while args and args[0] != "--":
        if args[0] == "--playing": playing = float(args[1]); args = args[2:]
        elif args[0] == "--paused": paused = float(args[1]); args = args[2:]
        else: sys.exit(__doc__)
    proc = subprocess.Popen([str(pathlib.Path(args[1]).resolve())] + args[2:])
    process = k32.OpenProcess(0x1000 | 0x0400 | 0x0010, False, proc.pid)
    try:
        time.sleep(5.0)
        total, rows = sample(process, proc.pid, playing)
        print(json.dumps({"state": "playing", "total_pct": round(total, 2), "threads": {k: round(v, 2) for k, v in rows.items() if v >= 0.05}}), flush=True)
        windows = []
        cb = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(lambda h, _: (windows.append(h) if u32.IsWindowVisible(h) else None) or True)
        pid = wt.DWORD()
        u32.EnumWindows(cb, 0)
        for hwnd in windows:
            u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == proc.pid:
                u32.PostMessageW(hwnd, 0x0100, 0x20, 0x00390001); u32.PostMessageW(hwnd, 0x0101, 0x20, 0xC0390001)
        time.sleep(2.0)
        total, rows = sample(process, proc.pid, paused)
        print(json.dumps({"state": "paused", "total_pct": round(total, 2), "threads": {k: round(v, 2) for k, v in rows.items() if v >= 0.01}}), flush=True)
    finally:
        proc.kill(); proc.wait()


if __name__ == "__main__":
    main()

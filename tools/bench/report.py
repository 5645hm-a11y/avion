"""Turns the head-to-head's raw results (build/bench/linux_results.jsonl, windows_results.jsonl) into the normalised figures
the site, whitepaper and cards use: build/bench/summary.json. Every derived number is computed here, from the raw lines, so
nothing on a page is typed by hand.  usage: python tools/bench/report.py"""
import json, pathlib, re

ROOT = pathlib.Path(__file__).resolve().parents[2]
BENCH = ROOT / "build" / "bench"


def lines(name):
    path = BENCH / name
    return [json.loads(l) for l in path.read_text(encoding="utf-8").splitlines() if l.strip().startswith("{")] if path.exists() else []


lin, win = lines("linux_results.jsonl"), lines("windows_results.jsonl")


def pick(rows, **want):
    return [r for r in rows if all(r.get(k) == v for k, v in want.items())]


def one(rows, **want):
    found = pick(rows, **want)
    return found[0] if found else None


summary = {"machine": {"linux": one(lin, bench="machine"), "windows": one(win, bench="machine")}, "scan": {}, "flow": {}, "survey": {}, "players": {}}

# Scene-cut and activity scan: AVION's inspector against ffmpeg's full decode + scene score, same files, same pictures.
for f in ("costa_rica_4k60_hdr.mp4", "lg_black_4k60_hdr.mp4"):
    s = {}
    a_lin = one(lin, bench="avion_scan", file=f, counted=False)
    a_cnt = one(lin, bench="avion_scan", file=f, counted=True)
    a_win = one(win, bench="avion_scan", file=f)
    if a_lin:
        s["codec"], s["size"], s["pictures"] = a_lin["codec"], f"{a_lin['width']}x{a_lin['height']}", a_lin["pictures"]
        s["avion_linux_us"], s["avion_linux_end_to_end_us"] = a_lin["inspect_us_mean"], a_lin["end_to_end_us_mean"]
        s["avion_linux_p99_us"] = a_lin["inspect_us_p99"]
    if a_cnt:
        s["avion_allocs_per_1000"] = a_cnt["allocations_inspect"] * 1000 / a_cnt["pictures"]
        s["avion_allocs_per_1000_with_reads"] = a_cnt["allocations_read_inspect"] * 1000 / a_cnt["pictures"]
    if a_win:
        s["avion_windows_us"], s["avion_windows_end_to_end_us"], s["avion_windows_p99_us"] = a_win["inspect_us_mean"], a_win["end_to_end_us_mean"], a_win["inspect_us_p99"]
        s["avion_candidates"], s["avion_confirmed_cuts"] = a_win["cuts"], a_win.get("confirmed_cuts", -1)
    for platform, rows in (("linux", lin), ("windows", win)):
        for r in pick(rows, bench="ffmpeg_scan", file=f):
            if r.get("counted") or not r["pictures"]:
                continue
            key = f"ffmpeg_{platform}_{r.get('decode', 'software')}_{'1t' if r['threads'] == 1 else 'mt'}"
            s[key + "_wall_us"] = r["rtime_s"] * 1e6 / r["pictures"]
            s[key + "_cpu_us"] = (r["utime_s"] + r["stime_s"]) * 1e6 / r["pictures"]
            s[key + "_cuts"] = r["cuts"]
    counted = sorted(pick(lin, bench="ffmpeg_scan", file=f, counted=True), key=lambda r: r["pictures"])
    if len(counted) >= 2 and counted[-1]["pictures"] > counted[0]["pictures"]:
        lo, hi = counted[0], counted[-1]
        # Whole allocations, half up: the pages print this figure in Python and JavaScript, which round halves differently.
        s["ffmpeg_allocs_per_1000"] = int((hi["allocations_total"] - lo["allocations_total"]) * 1000 / (hi["pictures"] - lo["pictures"]) + 0.5)
        s["ffmpeg_allocs_total_short"], s["ffmpeg_allocs_total_long"] = lo["allocations_total"], hi["allocations_total"]
    # Cut accuracy against scdet (ffmpeg's pixel-domain detector over the same 3,000 pictures): its cut times as picture
    # numbers, and AVION's final cuts (decode order) matched within 4 pictures, a hierarchical GOP's reordering.
    a = a_win or a_lin
    ff = [r for r in pick(lin, bench="ffmpeg_scan", file=f, counted=False) if "cut_times" in r]
    ff = max(ff, key=lambda r: r["pictures"]) if ff else None
    if a and ff and a.get("frame_100ns") and "final_cuts" in a:
        truth, found = [round(t * 1e7 / a["frame_100ns"]) for t in ff["cut_times"]], a["final_cuts"]
        matched = [c for c in truth if any(abs(x - c) <= 4 for x in found)]
        s["scdet_cuts"], s["avion_cuts"], s["cuts_matched"] = truth, found, len(matched)
        s["cuts_missed"] = [c for c in truth if c not in matched]
        s["cuts_extra"] = [x for x in found if all(abs(x - c) > 4 for c in truth)]
    if "avion_linux_us" in s and "ffmpeg_linux_software_1t_cpu_us" in s:
        s["speedup_cpu_linux_1t"] = s["ffmpeg_linux_software_1t_cpu_us"] / s["avion_linux_us"]
    if "avion_windows_us" in s and "ffmpeg_windows_software_mt_cpu_us" in s:
        s["speedup_cpu_windows"] = s["ffmpeg_windows_software_mt_cpu_us"] / s["avion_windows_us"]
    summary["scan"][f] = s

# Motion: AVION's whole perception step against OpenCV's dense flow, same 570 measured pictures.
fl = {}
for platform, rows in (("linux", lin), ("windows", win)):
    a = one(rows, bench="avion_flow", platform=f"{platform}-{'gcc' if platform == 'linux' else 'msvc'}")
    if a and not a.get("counted"):
        fl[f"avion_{platform}_ms"], fl[f"avion_{platform}_p99_ms"] = a["ms_mean"], a["ms_p99"]
c = [r for r in pick(lin, bench="avion_flow") if r.get("counted")]
if c:
    fl["avion_allocs_per_1000"] = c[0]["allocations"] * 1000 / c[0]["pictures"]
for r in pick(lin, bench="opencv_flow"):
    key = f"opencv_{r['method']}_{'1t' if r['threads'] == 1 else 'mt'}"
    if r.get("counted"):
        fl[key + "_allocs_per_1000"] = r["allocations"] * 1000 / r["pictures"]
    else:
        fl[key + "_ms"], fl[key + "_p99_ms"] = r["ms_mean"], r["ms_p99"]
        fl["opencv_version"] = r["opencv"]
summary["flow"] = fl

# The structure-tensor survey: AVION's at each SIMD level against OpenCV's Scharr + products + block means.
sv = {}
for platform, rows in (("linux", lin), ("windows", win)):
    for r in pick(rows, bench="avion_survey"):
        if not r["platform"].startswith(platform):
            continue
        for lv in r["levels"]:
            if r.get("counted"):
                sv[f"avion_{lv['level']}_allocs_per_1000"] = lv["allocations"] * 1000 / (5 * r["pictures"])
            else:
                sv[f"avion_{platform}_{lv['level']}_ms"] = lv["ms_mean"]
for r in pick(lin, bench="opencv_survey"):
    key = f"opencv_{'1t' if r['threads'] == 1 else 'mt'}"
    if r.get("counted"):
        sv[key + "_allocs_per_1000"] = r["allocations"] * 1000 / (5 * r["pictures"])
    else:
        sv[key + "_ms"] = r["ms_mean"]
summary["survey"] = sv

for r in pick(win, bench="player_cpu"):
    summary["players"][r["player"]] = {"playing_pct": r["playing_pct"], "paused_pct": r["paused_pct"]}

# The verification runs of this build, from their own logs (copied to build/bench when each ended): CTest's console output
# for Release and Debug, and the portable GCC suites' result lines (each "== suite flags" line followed by its pass line).
def ctest(name):
    path = BENCH / name
    if not path.exists():
        return None
    rows = re.findall(r"^\s*\d+/(\d+) Test\s+#\d+: (\S+) [ .]*(?:\*\*\*)?(Passed|Failed|Skipped|Timeout|Not Run)", path.read_text(encoding="utf-8", errors="replace"), re.M)
    seconds = re.findall(r"Total Test time \(real\) = ([\d.]+) sec", path.read_text(encoding="utf-8", errors="replace"))
    return {"total": int(rows[0][0]) if rows else 0, "passed": sum(r[2] == "Passed" for r in rows), "seconds": float(seconds[-1]) if seconds else None,
            "failed": sorted(r[1] for r in rows if r[2] in ("Failed", "Timeout", "Not Run")), "skipped": sorted(r[1] for r in rows if r[2] == "Skipped")}


suites = {"release": ctest("ctest_release.txt"), "debug": ctest("ctest_debug.txt")}
gcc = BENCH / "gcc_results.txt"
if gcc.exists():
    lines_ = gcc.read_text(encoding="utf-8", errors="replace").replace("﻿", "").splitlines()
    heads = [i for i, l in enumerate(lines_) if l.startswith("== ")]
    ok = [i for i in heads if i + 1 < len(lines_) and re.search(r"\b0 failures?\b|\b0 failure\(s\)|all passed", lines_[i + 1])
          and not re.search(r"\b[1-9]\d* failure|FAIL", lines_[i + 1])]
    suites["gcc"] = {"total": len(heads), "passed": len(ok)}
summary["suites"] = suites

# The run before the fixes (2026-11-02: build/bench/2026-11-02/summary.json), for the pages' before-and-after.
before = BENCH / "2026-11-02" / "summary.json"
if before.exists():
    b = json.loads(before.read_text(encoding="utf-8"))
    summary["previous"] = {"date": "2026-11-02", "players": b["players"],
                           "scan": {f: {k: v.get(k) for k in ("avion_candidates", "avion_confirmed_cuts", "ffmpeg_linux_software_mt_cuts")} for f, v in b["scan"].items()}}

(BENCH / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
print(json.dumps(summary, indent=2))

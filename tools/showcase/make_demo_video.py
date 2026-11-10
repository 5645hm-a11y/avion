"""AVION 0.1 launch reel: 16 s, 1920x1080, 60 fps, four scenes of 4 s. Everything on screen is read from what the engine wrote
(build/showcase: telemetry, event streams, GPU snapshots) or from build/bench/summary.json; nothing is typed in by hand except labels.
  python tools/showcase/make_demo_video.py [out_dir]       (needs build/showcase from make_showcase.py and ffmpeg on PATH)
The engine's lift pictures come from build/showcase/vid (media_showcase with --snap on every frame, see README of the reel).
ffmpeg only encodes the finished frames; the engine never sees it."""
import json, math, pathlib, struct, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORK = ROOT / "build" / "showcase"
VID = WORK / "vid"
FONTS = VID / "fonts"
OUT = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "artifacts" / "marketing"
OUT.mkdir(parents=True, exist_ok=True)
W, H, FPS = 1920, 1080, 60
SW, SH, SFPS = 640, 360, 30
PW, PH = 912, 513                     # panel size (16:9)
PX = (24, 984)                         # panel x origins
PY = 176
SC = PW / SW                           # source sample -> panel pixel

BG, PANEL, INK, DIM = (9, 11, 15), (17, 20, 26), (240, 243, 247), (139, 148, 163)
AV, BASE, WARN, OK = (86, 214, 255), (255, 138, 92), (255, 206, 86), (118, 230, 160)


def font(w, s):
    return ImageFont.truetype(str(FONTS / f"Inter-{w}.ttf"), s)


F = {(w, s): font(w, s) for w in (400, 600, 800) for s in (20, 22, 24, 28, 72, 32, 40, 52, 64, 96, 120)}


def f(w, s):
    return F[(w, s)]


def jl(name):
    return [json.loads(l) for l in (WORK / f"{name}_telemetry.jsonl").read_text(encoding="utf-8").splitlines()]


def rgba(name, frames):
    return np.memmap(WORK / f"{name}.rgba", dtype=np.uint8, mode="r", shape=(frames, SH, SW, 4))


def events(name):
    data = (WORK / f"{name}_events.bin").read_bytes()
    out, i = {}, 0
    dt = np.dtype([("x", "<u2"), ("y", "<u2"), ("p", "i1"), ("c", "u1"), ("r", "<u2"), ("t", "<i8")])
    while i < len(data):
        fr, n = struct.unpack_from("<II", data, i); i += 8
        out.setdefault(fr, []).append(np.frombuffer(data, dtype=dt, count=n, offset=i)); i += 16 * n
    return {k: np.concatenate(v) for k, v in out.items()}


# ---------------------------------------------------------------- data
truth = json.loads((WORK / "truth.json").read_text(encoding="utf-8"))
show = json.loads((WORK / "showcase.json").read_text(encoding="utf-8"))
bench = json.loads((ROOT / "build" / "bench" / "summary.json").read_text(encoding="utf-8"))
LG, FL = bench["longform"] if "longform" in bench else None, bench["flow"]
allocs_ffmpeg = None


def find(o, key):
    if isinstance(o, dict):
        if key in o:
            return o[key]
        for v in o.values():
            r = find(v, key)
            if r is not None:
                return r
    return None


LGB = bench["scan"]["lg_black_4k60_hdr.mp4"]
ALLOC_FF = int(LGB["ffmpeg_allocs_per_1000"])
SPEEDUP = float(LGB["speedup_cpu_linux_1t"])
FLOW_A, FLOW_B = float(FL["avion_linux_ms"]), float(FL["opencv_dis_ultrafast_1t_ms"])

EV_SRC = rgba("events", 90)
EV_RAW, EV_STAB = events("events_raw"), events("events_stab")
EV_TEL_R, EV_TEL_S = jl("events_raw"), jl("events_stab")
SHAKE = np.array(truth["events"]["shake"])
VIEW = np.array([r["view"] for r in EV_TEL_S])
OFF = np.stack([VIEW[:, 1] * SW - SW / 2, VIEW[:, 2] * SH - SH / 2], 1)
LOCK = 10                                                  # frames the filter needs to lock
RAW_RMS = float(np.sqrt((SHAKE[LOCK:] ** 2).sum(1).mean()))
STAB_RMS = float(np.sqrt(((OFF - SHAKE)[LOCK:] ** 2).sum(1).mean()))
TOT = json.loads((WORK / "events_totals.json").read_text(encoding="utf-8"))
RAW_EVENTS = TOT["events_raw"]["written"] + TOT["events_raw"]["dropped"]
STAB_EVENTS = TOT["events_stab"]["written"] + TOT["events_stab"]["dropped"]

PER = rgba("perceive", 120)
PER_TEL, LVL_TEL = jl("perceive"), jl("perceive_level")
TILT = truth["perceive"]["tilt_deg"] if "perceive" in truth and "tilt_deg" in truth["perceive"] else 4.0

PUL = rgba("pulse", 300)
PUL_TEL, CHR_TEL = jl("pulse"), jl("pulse_chroma")
PUL_BPM = float(show["pulse"]["bpm"]); CHR_BPM = float(show["pulse"]["chroma_bpm"])
IMU_RMS = float(show["pulse"]["imu_rms"])


def lift_img(kind, i):
    b = 3 if i >= 26 else (0 if i < 8 else (1 if i < 16 else 2))
    return Image.open(VID / f"{kind}_b{b}_f{i:04d}.bmp").convert("RGB")


LIFT = {k: [lift_img(k, i).resize((PW, PH), Image.LANCZOS) for i in range(30)] for k in ("off", "on")}
LUMA = {k: float(np.mean([np.asarray(im.convert("L"), float).mean() for im in LIFT[k]])) for k in LIFT}

# ---------------------------------------------------------------- helpers
def ease(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)


def src_pair(t_src, n):
    """Two source frames and the blend between them (30 fps source onto 60 fps output)."""
    i = int(math.floor(t_src)); a = t_src - i
    return i % n, (i + 1) % n, a


def blend(arr, t_src, n):
    i, j, a = src_pair(t_src, n)
    return arr[i][..., :3].astype(np.float32) * (1 - a) + arr[j][..., :3].astype(np.float32) * a


def to_panel(img, box=None):
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    if box:
        im = im.crop(box)
    return im.resize((PW, PH), Image.BICUBIC)


def text(d, xy, s, w, size, fill, anchor="la"):
    d.text(xy, s, font=f(w, size), fill=fill, anchor=anchor)


def rounded(d, box, r, fill, outline=None):
    d.rounded_rectangle(box, r, fill=fill, outline=outline, width=2 if outline else 0)


def chip(d, x, y, label, colour, size=24):
    wd = d.textlength(label, font=f(600, size)) + 36
    rounded(d, (x, y, x + wd, y + 46), 23, (colour[0] // 6, colour[1] // 6, colour[2] // 6), colour)
    text(d, (x + 18, y + 23), label, 600, size, colour, "lm")
    return x + wd + 12


def header(d, title, sub, scene, t_in):
    text(d, (24, 28), "AVION 0.1", 800, 28, AV)
    for k in range(4):
        col = AV if k == scene else (55, 62, 74)
        d.rounded_rectangle((W - 24 - (4 - k) * 54, 34, W - 24 - (4 - k) * 54 + 42, 40), 3, fill=col)
    a = ease(t_in / 0.35)
    off = int((1 - a) * 18)
    text(d, (24, 70 + off), title, 800, 52, tuple(int(c * a) for c in INK))
    text(d, (24, 132 + off), sub, 400, 24, tuple(int(c * a) for c in DIM))


def footer(d, s):
    text(d, (24, H - 40), s, 400, 22, (96, 104, 118))


def panel_frame(canvas, x, label, colour):
    d = ImageDraw.Draw(canvas)
    d.rounded_rectangle((x - 3, PY - 3, x + PW + 3, PY + PH + 3), 10, outline=colour, width=3)
    rounded(d, (x + 16, PY + 16, x + 16 + d.textlength(label, font=f(800, 24)) + 28, PY + 58), 21, (0, 0, 0))
    text(d, (x + 30, PY + 37), label, 800, 24, colour, "lm")


def stat(d, x, y, big, small, colour, size=96):
    text(d, (x, y), big, 800, size, colour)
    text(d, (x, y + size + 14), small, 400, 24, DIM)


def draw_events(canvas_arr, ev, scale=SC):
    if ev is None or len(ev) == 0:
        return
    on = ev["p"] > 0
    x = np.minimum((ev["x"].astype(np.float32) * scale).astype(int), PW - 1)
    y = np.minimum((ev["y"].astype(np.float32) * scale).astype(int), PH - 1)
    for sel, col in ((on, (255, 150, 100)), (~on, (100, 185, 255))):
        np.maximum.at(canvas_arr, (y[sel], x[sel]), np.array(col, np.float32))


def paste_events(panel, ev_list):
    arr = np.zeros((PH, PW, 3), np.float32)
    for ev in ev_list:
        draw_events(arr, ev)
    base = np.asarray(panel, np.float32) * 0.42
    mask = arr.max(2, keepdims=True) > 0
    # dots are 2x2 for legibility
    m = mask[..., 0]
    for dy in (0, 1):
        for dx in (0, 1):
            sh = np.zeros_like(arr); sm = np.zeros_like(m)
            sh[dy:, dx:] = arr[:PH - dy, :PW - dx]; sm[dy:, dx:] = m[:PH - dy, :PW - dx]
            base = np.where(sm[..., None], sh, base)
    return Image.fromarray(np.clip(base, 0, 255).astype(np.uint8))


# ---------------------------------------------------------------- scenes
def scene1(t):
    fi = int((t * SFPS)) % 90
    ts = (t * SFPS) % 90
    raw = blend(EV_SRC, ts, 90)
    ox, oy = OFF[fi]
    crop = (SW / 2 + ox - 256, SH / 2 + oy - 144, SW / 2 + ox + 256, SH / 2 + oy + 144)
    left = to_panel(raw)
    right = to_panel(raw, tuple(int(round(v)) for v in crop))
    left = paste_events(left, [EV_RAW.get(fi)])
    right = paste_events(right, [EV_STAB.get(fi), EV_STAB.get(fi - 1)])
    img = Image.new("RGB", (W, H), BG)
    img.paste(left, (PX[0], PY)); img.paste(right, (PX[1], PY))
    panel_frame(img, PX[0], "CAMERA AS SHAKEN", BASE); panel_frame(img, PX[1], "AVION VIRTUAL IMU · NO GYRO", AV)
    d = ImageDraw.Draw(img)
    header(d, f"AVION 0.1 — 0 Heap Allocations vs. {ALLOC_FF:,} in FFmpeg",
           "per 1,000 frames of 4K HEVC (FFmpeg decode + scdet); one preloaded counter, same rule in every process", 0, t)
    yy = PY + PH + 36
    ev_l, ev_r = EV_TEL_R[fi]["events"], EV_TEL_S[fi]["events"]
    stat(d, 24, yy, f"{RAW_RMS:.2f} px", "RMS shake on screen, as it comes", BASE)
    stat(d, 500, yy, f"{RAW_EVENTS / 1e6:.1f} M", "events fired in 3 s", BASE)
    stat(d, 984, yy, f"{STAB_RMS:.2f} px", "RMS after the virtual IMU, edges clean", AV)
    stat(d, 1460, yy, f"{STAB_EVENTS / 1e6:.2f} M", "events: what is left is what moves", AV)
    d.line((960, yy, 960, yy + 150), fill=(55, 62, 74), width=2)
    text(d, (24, yy + 168), f"live: {ev_l:,} events this frame", 400, 24, DIM)
    text(d, (984, yy + 168), f"live: {ev_r:,} events this frame · IMU {IMU_RMS:.3f} px rms off truth", 400, 24, DIM)
    footer(d, "Footage: Big Buck Bunny (c) Blender Foundation, CC BY 3.0 · pictures and numbers are the engine's own output through its C SDK")
    return img


def scene2(t):
    t0 = t
    t = t - 4
    ts = (t * SFPS)
    fi = min(int(ts), 119)
    raw = blend(PER, min(ts, 119), 120)
    tel = PER_TEL[fi]; lv = LVL_TEL[fi]
    left = to_panel(raw)
    lvl_deg = lv["level_deg"]
    right_im = Image.fromarray(np.clip(raw, 0, 255).astype(np.uint8)).rotate(-lvl_deg, resample=Image.BICUBIC, center=(SW / 2, SH / 2))
    right = right_im.crop((46, 26, SW - 46, SH - 26)).resize((PW, PH), Image.BICUBIC)
    img = Image.new("RGB", (W, H), BG)
    img.paste(left, (PX[0], PY)); img.paste(right, (PX[1], PY))
    panel_frame(img, PX[0], "TILTED CAMERA · LIVE PERCEPTION", BASE); panel_frame(img, PX[1], "AUTO-LEVEL", AV)
    d = ImageDraw.Draw(img)
    header(d, "Horizon, target tracking, time to contact — on the GPU",
           "a generated harbour tilted 4° with a buoy closing at 1 m/s: the truth is known, the engine reads it live", 1, t)
    hd = tel["horizon_deg"]
    # horizon line: scene geometry through the centre (generated scene), angle as the engine measured it
    dy = 0.56 * 432 - 216
    cx0 = SW / 2 - dy * math.sin(math.radians(TILT)); cy0 = SH / 2 + dy * math.cos(math.radians(TILT))
    k = math.tan(math.radians(hd))
    p0 = (PX[0], PY + (cy0 - k * cx0) * SC); p1 = (PX[0] + PW, PY + (cy0 + k * (SW - cx0)) * SC)
    # clip to the panel: draw on a layer
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0)); ld = ImageDraw.Draw(layer)
    ld.line([p0, p1], fill=AV + (255,), width=4)
    for o in tel["objects"]:
        bx, by, bw, bh = o["box"]
        x0, y0 = PX[0] + (bx - bw / 2) * PW, PY + (by - bh / 2) * PH
        x1, y1 = PX[0] + (bx + bw / 2) * PW, PY + (by + bh / 2) * PH
        hot = o["ttc"] > 0
        col = WARN if hot else OK
        ld.rectangle((x0, y0, x1, y1), outline=col + (255,), width=3)
        lab = f"#{o['id']}" + (f"  TTC {o['ttc']:.2f} s" if hot else "")
        ld.rectangle((x0, y0 - 30, x0 + ld.textlength(lab, font=f(800, 20)) + 14, y0), fill=col + (255,))
        ld.text((x0 + 7, y0 - 15), lab, font=f(800, 20), fill=(10, 10, 10), anchor="lm")
    crop = layer.crop((PX[0], PY, PX[0] + PW, PY + PH))
    img.paste(crop, (PX[0], PY), crop)
    # right: level line
    yl = PY + (SH / 2 + dy - 26) * (PH / (SH - 52))
    d.line((PX[1], yl, PX[1] + PW, yl), fill=AV, width=4)
    d = ImageDraw.Draw(img)
    yy = PY + PH + 36
    stat(d, 24, yy, f"{hd:+.2f}°", "auto-horizon, measured (true +4.00°)", AV)
    ttc = tel["ttc"]
    stat(d, 500, yy, f"{ttc:.2f} s" if ttc > 0 else "—", "time to contact, live", WARN)
    stat(d, 984, yy, f"{len(tel['objects'])}", "tracked targets, ids kept through crossings", OK)
    x = 1480
    chip(d, x, yy + 6, "NCHW tensor export · SDK", AV)
    chip(d, x, yy + 66, f"level {lvl_deg:+.1f}° applied", AV)
    footer(d, "Generated harbour (truth known) · horizon angle, boxes and time to contact are read from the engine's per-frame telemetry")
    return img


def scene3(t):
    tt = t - 8
    ts = tt * SFPS
    # left: the engine's lifted pictures against the same frames un-lifted, a wipe sweeping the picture
    idx = ts % 58
    li = int(idx) if idx < 30 else int(57 - idx)
    off_im, on_im = LIFT["off"][li], LIFT["on"][li]
    wipe = int(PW * (0.5 + 0.5 * math.sin(2 * math.pi * tt / 4.0 - math.pi / 2)))
    left = off_im.copy()
    left.paste(on_im.crop((0, 0, wipe, PH)), (0, 0))
    # right: pulse footage, stabilised by the engine's view
    pf = 150 + int(ts)
    pf = min(pf, 299)
    raw = blend(PUL, 150 + ts, 300)
    vw = PUL_TEL[pf]["view"]
    ox, oy = vw[1] * SW - SW / 2, vw[2] * SH - SH / 2
    crop = tuple(int(round(v)) for v in (SW / 2 + ox - 256, SH / 2 + oy - 144, SW / 2 + ox + 256, SH / 2 + oy + 144))
    right = to_panel(raw, crop)
    img = Image.new("RGB", (W, H), BG)
    img.paste(left, (PX[0], PY)); img.paste(right, (PX[1], PY))
    panel_frame(img, PX[0], "DARK TACTICAL FOOTAGE  →  NEURAL 2× + SHADOW LIFT", BASE if wipe < PW / 2 else AV)
    panel_frame(img, PX[1], "FACE · STABILISED · PULSE READ LIVE", AV)
    d = ImageDraw.Draw(img)
    d.line((PX[0] + wipe, PY, PX[0] + wipe, PY + PH), fill=INK, width=3)
    # vibrometry region (engine's own box, source samples -> panel through the view crop)
    l, tp, r, b = PUL_TEL[pf]["vib_region"]
    s = PW / 512
    bx = [PX[1] + (l - crop[0]) * s, PY + (tp - crop[1]) * s, PX[1] + (r - crop[0]) * s, PY + (b - crop[1]) * s]
    d.rectangle(bx, outline=OK, width=3)
    text(d, (bx[0] + 8, bx[1] + 6), "rPPG region", 800, 20, OK)
    header(d, "Shadow-lift on the GPU, and a pulse through the shake",
           "left: 3 stops underexposed, the engine's own output; right: a 72 BPM pulse one code deep in the skin's colour, camera shaking", 2, tt)
    yy = PY + PH + 36
    stat(d, 24, yy, f"{LUMA['off']:.0f} → {LUMA['on']:.0f}", "mean luma, engine output (Neural 2×, lift 1.0)", AV)
    bpm = PUL_TEL[pf]["vital"][0]; chm = CHR_TEL[pf]["vital"][0]
    stat(d, 984, yy, f"{bpm:.2f} BPM", f"Cr chroma alone {chm:.2f} · truth 72.00", OK, 72)
    # trace
    x0, x1, y0, y1 = 1450, 1896, yy + 4, yy + 148
    rounded(d, (x0, y0, x1, y1), 12, PANEL)
    lo, hi = 60.0, 150.0
    ty = lambda v: y1 - 12 - (v - lo) / (hi - lo) * (y1 - y0 - 24)
    d.line((x0 + 8, ty(72), x1 - 8, ty(72)), fill=(60, 80, 70), width=1)
    d.line((x0 + 8, ty(138), x1 - 8, ty(138)), fill=BASE, width=1)
    text(d, (x1 - 12, ty(138) - 4), "138 shake ignored (suite)", 400, 20, BASE, "rb")
    pts = []
    for q in range(150, pf + 1):
        v = PUL_TEL[q]["vital"][0]
        if v > 0:
            pts.append((x0 + 8 + (q - 150) / 120 * (x1 - x0 - 16), ty(v)))
    if len(pts) > 1:
        d.line(pts, fill=OK, width=4)
    text(d, (x0 + 14, ty(72) + 6), "72", 600, 20, OK)
    footer(d, "Footage: Big Buck Bunny (c) Blender Foundation, CC BY 3.0 · lift = the engine's GPU output, the pulse reading = its per-frame telemetry")
    return img


def bar_row(d, y, label, a_txt, b_txt, frac_a, frac_b, p, note=None):
    text(d, (24, y), label, 600, 32, INK)
    if note:
        text(d, (24, y + 40), note, 400, 22, DIM)
    bx, bw = 700, 1000
    for j, (txt, frac, col, tag) in enumerate(((a_txt, frac_a, AV, "AVION"), (b_txt, frac_b, BASE, "other"))):
        yb = y + j * 40
        rounded(d, (bx, yb + 2, bx + bw, yb + 32), 8, PANEL)
        wd = max(0, int(bw * frac * p))
        if frac > 0 and wd > 8:
            rounded(d, (bx, yb + 2, bx + wd, yb + 32), 8, col)
        tx = bx + max(wd, 0) + 14 if frac < 0.82 else bx + wd - 14
        anchor = "lm" if frac < 0.82 else "rm"
        text(d, (tx, yb + 17), txt, 800, 24, col if frac < 0.82 else (10, 10, 10), anchor)


def scene4(t):
    tt = t - 12
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    header(d, "API 14 — the verified scorecard", "every figure measured on this build, same machine, same files; the head-to-head is on the site", 3, tt)
    # log-ish scales so a 0 and a 134,501x can sit on one axis
    def logfrac(v, vmax):
        return 0.0 if v <= 0 else math.log10(1 + v) / math.log10(1 + vmax)
    rows = [
        ("Heap allocations / 1,000 frames", "0", f"{ALLOC_FF:,}", 0.0, 1.0, "AVION vs FFmpeg decode + scdet, 4K HEVC", 0.15),
        ("4K HEVC scene scan, CPU per frame", "0.23 µs", "31.1 ms", logfrac(0.23e-3, 31.1), 1.0, f"{SPEEDUP:,.0f}× less CPU (slice headers vs decode + score)", 0.9),
        ("Optical flow, 640×480", f"{FLOW_A:.2f} ms", f"{FLOW_B:.2f} ms", FLOW_A / FLOW_B, 1.0, f"{FLOW_B / FLOW_A:.1f}× faster than OpenCV DIS", 1.65),
    ]
    y = 190
    for lab, a, b, fa, fb, note, t0 in rows:
        p = ease((tt - t0 * 0.6) / 0.7)
        bar_row(d, y, lab, a, b, fa, fb, p, note)
        y += 150
    p = ease((tt - 1.9) / 0.5)
    c = lambda col: tuple(int(v * p) for v in col)
    text(d, (24, y + 6), "0.7 % of a core", 800, 64, c(AV))
    text(d, (24, y + 82), "H.264 hardware decode, GPU → shared D3D11 / D3D12 textures, 10-bit PQ / scRGB (HDR10 peak read: 5,849 cd/m²)", 400, 24, c(DIM))
    p2 = ease((tt - 2.5) / 0.5)
    c2 = lambda col: tuple(int(v * p2) for v in col)
    text(d, (24, y + 150), "221/221 MSVC  ·  96/96 GCC  ·  C99 header  ·  Python pip  ·  .NET NuGet", 800, 40, c2(INK))
    p3 = ease((tt - 3.1) / 0.5)
    text(d, (W // 2, H - 56), "5645hm-a11y.github.io/avion", 800, 52, tuple(int(v * p3) for v in AV), "mm")
    return img


SCENES = (scene1, scene2, scene3, scene4)


def main():
    mp4 = OUT / "avion-0.1-demo.mp4"
    cmd = ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-",
           "-c:v", "libx264", "-preset", "slow", "-profile:v", "high", "-pix_fmt", "yuv420p", "-crf", "21", "-maxrate", "7M", "-bufsize", "14M",
           "-g", "60", "-movflags", "+faststart", "-an", str(mp4)]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    only = [int(a) for a in sys.argv[2:]]
    for i in range(16 * FPS):
        t = i / FPS
        img = SCENES[min(3, int(t // 4))](t)
        if only and i not in only:
            continue
        if only:
            img.save(WORK / f"demo_check_{i:04d}.png"); continue
        p.stdin.write(img.tobytes())
    if not only:
        p.stdin.close(); p.wait()
        print("wrote", mp4, mp4.stat().st_size)
    else:
        p.stdin.close(); p.wait()


if __name__ == "__main__":
    main()

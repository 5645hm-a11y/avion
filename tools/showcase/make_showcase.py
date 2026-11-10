"""The site's visual showcase: real engine output on real footage with a known truth (docs/site, "What it sees").
  python tools/showcase/make_showcase.py [extract] [scenes] [run] [compose]   (no argument: all four)
- extract: clips of Big Buck Bunny (c) Blender Foundation, CC BY 3.0 (assets/test_fixtures/big_buck_bunny_4k60.mp4) to raw RGBA with
  ffmpeg (input preparation only: the engine never sees ffmpeg's code, only the frames);
- scenes: what the camera saw, with its truth: underexposed by 3 stops; tilted 4 degrees with an obstacle approaching at a known
  speed; shaken; a pulse at 72 BPM added to the skin's colour (about 1 code: invisible) under a shake;
- run: build/Release/media_showcase (tools/showcase/avion_showcase.c, the C SDK in host mode) on each: snapshots, telemetry, events;
- compose: lays the engine's snapshots and the boxes, lines and spikes it reported into the site's pictures
  (docs/site/assets/showcase/*.webp, the cards' *.png) and build/showcase/showcase.json for the pages' captions and charts.
Nothing measured here is computed by this script: every number on the pictures is read from what the engine wrote."""
import json, math, pathlib, struct, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORK = ROOT / "build" / "showcase"
OUT = ROOT / "docs" / "site" / "assets" / "showcase"
BBB = ROOT / "assets" / "test_fixtures" / "big_buck_bunny_4k60.mp4"
EXE = ROOT / "build" / "Release" / "media_showcase.exe"
W, H, FPS = 640, 360, 30
STAGES = set(sys.argv[1:]) or {"extract", "scenes", "run", "compose"}
WORK.mkdir(parents=True, exist_ok=True); OUT.mkdir(parents=True, exist_ok=True)

# Each clip: start second, frames at 30/s, extracted size (larger than 640 x 360 where the scene moves or turns the picture).
CLIPS = {"forest": (380.5, 30, (W, H)), "meadow": (118.0, 120, (768, 432)), "rodents": (140.0, 90, (704, 396)), "face": (200.0, 1, (704, 396))}


def extract():
    for name, (start, frames, (w, h)) in CLIPS.items():
        out = WORK / f"src_{name}.rgba"
        if out.exists() and out.stat().st_size == w * h * 4 * frames:
            continue
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", str(start), "-i", str(BBB), "-frames:v", str(frames), "-vf",
                        f"fps={FPS},scale={w}:{h}:flags=lanczos", "-pix_fmt", "rgba", "-f", "rawvideo", str(out)], check=True)
        print("extracted", name)


def clip(name):
    _, frames, (w, h) = CLIPS[name]
    return np.fromfile(WORK / f"src_{name}.rgba", dtype=np.uint8).reshape(frames, h, w, 4)


def shift(img, dx, dy):
    """img (h, w, 4) moved by (dx, dy) samples, bilinear, cropped to W x H about the centre: the camera's shake."""
    h, w = img.shape[:2]
    x0, y0 = (w - W) / 2 - dx, (h - H) / 2 - dy
    xs, ys = np.arange(W) + x0, np.arange(H) + y0
    ix, iy = np.floor(xs).astype(int), np.floor(ys).astype(int)
    fx, fy = (xs - ix)[None, :, None], (ys - iy)[:, None, None]
    a = img.astype(np.float32)
    p00, p01 = a[iy][:, ix], a[iy][:, ix + 1]
    p10, p11 = a[iy + 1][:, ix], a[iy + 1][:, ix + 1]
    return (p00 * (1 - fx) * (1 - fy) + p01 * fx * (1 - fy) + p10 * (1 - fx) * fy + p11 * fx * fy)


def shake(t, k=2.0):
    """The camera's shake at time t (samples, +x right, +y down): the vibrometry suite's, k times larger (640 samples across)."""
    return (k * (3.0 * math.sin(2 * math.pi * 2.3 * t) + 1.5 * math.sin(2 * math.pi * 5.1 * t + 1.0)), k * 2.5 * math.sin(2 * math.pi * 3.7 * t + 0.5))


def noise(rng, shape, span=1):
    return rng.integers(-span, span + 1, size=shape).astype(np.float32)


def save(name, frames):
    arr = np.clip(np.rint(frames), 0, 255).astype(np.uint8)
    arr[..., 3] = 255
    arr.tofile(WORK / f"{name}.rgba")
    return len(arr)


def obstacle_texture(u, v):
    """A boulder's surface at (u, v) in its own radii: smooth value noise in four octaves, so its flow is measurable at any size."""
    out = np.zeros_like(u)
    for k, amp in ((3, .5), (7, .27), (15, .15), (31, .08)):
        x, y = (u + 1.7) * k, (v + 2.3) * k
        ix, iy = np.floor(x), np.floor(y)
        fx, fy = x - ix, y - iy
        def r(i, j):
            n = (i * 374761393 + j * 668265263 + k * 1442695041).astype(np.int64) & 0xFFFFFFFF
            n = (n ^ (n >> 13)) * 1274126177 & 0xFFFFFFFF
            return ((n ^ (n >> 16)) & 0xFFFF) / 65535.0
        sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
        out += amp * ((r(ix, iy) * (1 - sx) + r(ix + 1, iy) * sx) * (1 - sy) + (r(ix, iy + 1) * (1 - sx) + r(ix + 1, iy + 1) * sx) * sy)
    return out


def turned(img, tilt):
    """img (h, w, 3 or 4) turned `tilt` degrees clockwise about its centre, the centre W x H kept (no corner left empty)."""
    rgba = img if img.shape[2] == 4 else np.dstack([img, np.full(img.shape[:2], 255, np.float32)])
    out = np.asarray(Image.fromarray(np.clip(np.rint(rgba), 0, 255).astype(np.uint8)).rotate(-tilt, resample=Image.BICUBIC))  # PIL: + is counter-clockwise.
    h, w = out.shape[:2]
    return out[(h - H) // 2:(h - H) // 2 + H, (w - W) // 2:(w - W) // 2 + W].astype(np.float32)


def harbour(t, w=768, h=432, seed=7):
    """A harbour at dusk at time t: sky, a sea horizon at 0.56 h with slow swell, two shores of buildings with lit windows,
    a boat crossing at 40 samples a second."""
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    hy = 0.56 * h
    sky = np.stack([70 + 150 * (yy / hy), 80 + 95 * (yy / hy), 120 + 60 * (yy / hy)], -1)
    d = np.clip((yy - hy) / (h - hy), 0, 1)
    swell = 6 * np.sin(yy * 0.3 + np.sin(xx * 0.02)) * (0.3 + d)
    img = np.where((yy < hy)[..., None], sky, np.stack([30 + 25 * d + swell * .6, 55 + 30 * d + swell * .8, 80 + 30 * d + swell], -1))
    b = np.random.default_rng(seed)
    x = 0
    while x < w:
        bw = int(b.integers(26, 70)); bh = int(b.integers(30, 150)) if (x < 0.38 * w or x > 0.7 * w) else int(b.integers(0, 12))
        if bh > 0:
            top, shade = int(hy - bh), b.uniform(28, 55)
            img[top:int(hy), x:x + bw] = [shade, shade * 0.95, shade * 1.1]
            for wy in range(top + 6, int(hy) - 6, 11):
                for wx in range(x + 5, x + bw - 6, 9):
                    if b.random() < 0.55:
                        img[wy:wy + 5, wx:wx + 4] = [235, 200, 120]
        x += bw + int(b.integers(2, 8))
    bx, by = int(0.42 * w + 40 * t), int(hy + 22)
    img[by:by + 10, bx:bx + 64] = [200, 190, 175]
    img[by - 12:by, bx + 18:bx + 44] = [225, 220, 210]
    img[by - 9:by - 4, bx + 22:bx + 40:6] = [60, 80, 110]
    return img


def scenes():
    rng = np.random.default_rng(2026)
    truth = {}
    # 1. Underexposed by 3 stops (linear light x 1/8): the forest at dusk, as a cheap camera would record it.
    src = clip("forest").astype(np.float32) / 255
    dark = np.power(np.power(src, 2.2) / 8.0, 1 / 2.2) * 255
    dark[..., :3] += noise(rng, dark[..., :3].shape)
    truth["lift"] = {"frames": save("lift", dark), "stops": 3}
    # 2. A harbour at dusk (generated: its lines and its truth known), tilted +4 degrees (clockwise: the horizon's right end lower),
    #    a boat crossing and a buoy drifting toward the camera from 5 m at 1 m/s (TTC 5 s -> 1 s); and the same untilted.
    z0, speed, r0, cx, cy = 5.0, 1.0, 14.0, 0.3 * W, 0.76 * H
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    for scene, tilt in (("perceive", 4.0), ("perceive_flat", 0.0)):
        frames, ttc = [], []
        for f in range(120):
            t = f / FPS
            pic = turned(harbour(t), tilt)
            z = z0 - speed * t
            r = r0 * z0 / z
            u, v = (xx - cx) / r, (yy - cy) / r
            rho2 = u * u + v * v
            shade = np.sqrt(np.clip(1 - rho2, 0, 1)) * 0.6 + 0.4
            tex = obstacle_texture(u * z0 / 2, v * z0 / 2)  # The texture is the surface's: it grows with the buoy.
            buoy = np.stack([170 + 85 * tex, 70 + 60 * tex, 40 + 40 * tex], -1) * shade[..., None]
            edge = np.clip((1 - np.sqrt(rho2)) * r, 0, 1)[..., None]  # One sample of anti-aliasing at the rim.
            pic[..., :3] = np.where((rho2 < 1)[..., None], pic[..., :3] * (1 - edge) + buoy * edge, pic[..., :3])
            pic[..., :3] += noise(rng, pic[..., :3].shape)
            frames.append(pic); ttc.append(z / speed)
        truth[scene] = {"frames": save(scene, np.stack(frames)), "tilt_deg": tilt, "ttc": ttc, "obstacle": [cx / W, cy / H], "z0_m": z0, "speed_mps": speed}
    # The meadow of Big Buck Bunny tilted the same way: its grass and hills hold no level line (the limits section reports it).
    truth["meadow"] = {"frames": save("meadow", np.stack([turned(im, 4.0) + noise(rng, (H, W, 4)) for im in clip("meadow")])), "tilt_deg": 4.0}
    # 3. Shaken: the rodents' scene as a hand-held camera saw it (6 + 3 samples across at 2.3 and 5.1 Hz, 5 down at 3.7 Hz).
    src = clip("rodents")
    frames, shakes = [], []
    for f, im in enumerate(src):
        dx, dy = shake(f / FPS)
        pic = shift(im, dx, dy); pic[..., :3] += noise(rng, pic[..., :3].shape)
        frames.append(pic); shakes.append([dx, dy])
    truth["events"] = {"frames": save("events", np.stack(frames)), "shake": shakes}
    # 4. A pulse at 72 BPM in the skin's colour (green -1.2, red -0.4, blue -0.2 codes at the peak: blood absorbs green) over the
    #    face, the camera shaken, +-1 code of sensor noise: 10 s.
    still = clip("face")[0]
    hh, ww = still.shape[:2]
    gy, gx = np.mgrid[0:hh, 0:ww].astype(np.float32)
    face = np.exp(-(((gx - ww * 0.5) / (ww * 0.26)) ** 2 + ((gy - hh * 0.47) / (hh * 0.3)) ** 2) ** 2)[..., None]
    # The same as pure chroma: Cr +-0.8 code with Y' and Cb unchanged (BT.709: red +1.43, green -0.43 codes).
    for scene, pulse in (("pulse", np.array([-0.4, -1.2, -0.2, 0], np.float32)), ("pulse_chroma", np.array([1.434, -0.426, 0, 0], np.float32))):
        frames, shakes = [], []
        for f in range(300):
            t = f / FPS
            beat = math.sin(2 * math.pi * 1.2 * t)
            im = still.astype(np.float32) + face * pulse * beat
            dx, dy = shake(t, 1.5)
            pic = shift(im, dx, dy); pic[..., :3] += noise(rng, pic[..., :3].shape)
            frames.append(pic); shakes.append([dx, dy])
        truth[scene] = {"frames": save(scene, np.stack(frames)), "bpm": 72.0, "shake": shakes, "pulse_rgb": pulse[:3].tolist()}
    (WORK / "truth.json").write_text(json.dumps(truth), encoding="utf-8")


RUNS = {
    # name: (scene, options)
    "lift_off": ("lift", ["--gpu", "1280", "720", "--recon", "bilinear", "--snap", "24"]),
    "lift_on": ("lift", ["--gpu", "1280", "720", "--recon", "neural", "--lift", "1.0", "--snap", "24"]),
    "perceive": ("perceive", ["--gpu", "1280", "720", "--snap", "60", "--snap", "90", "--snap", "110"]),
    "perceive_level": ("perceive", ["--gpu", "1280", "720", "--auto-level", "--snap", "110"]),
    "events_raw": ("events", ["--events", "raw", "--snap", "60"]),
    "events_stab": ("events", ["--events", "stab", "--stabilize", "--snap", "60"]),
    "perceive_flat": ("perceive_flat", []),
    "meadow": ("meadow", []),
    "pulse": ("pulse", ["--gpu", "1280", "720", "--stabilize", "--snap", "299"]),
    "pulse_chroma": ("pulse_chroma", ["--stabilize"]),
}


def run(only=None):
    truth = json.loads((WORK / "truth.json").read_text(encoding="utf-8"))
    totals_path = WORK / "events_totals.json"
    totals = json.loads(totals_path.read_text(encoding="utf-8")) if totals_path.exists() else {}
    for name, (scene, opts) in RUNS.items():
        if only and name not in only:
            continue
        # Relative paths: the SDK takes UTF-8, argv arrives in the ANSI code page.
        cmd = [str(EXE), f"{scene}.rgba", name, str(W), str(H), str(FPS), str(truth[scene]["frames"])] + opts
        line = subprocess.run(cmd, cwd=WORK, check=True, capture_output=True, text=True).stdout.strip()
        print(line)
        # "name: N frames, E events (dropped D, sensor W x H)": the event camera's own counts (media_query_event_camera).
        if "--events" in opts:
            words = line.replace(",", " ").replace("(", " ").split()
            totals[name] = {"written": int(words[words.index("events") - 1]), "dropped": int(words[words.index("dropped") + 1])}
    totals_path.write_text(json.dumps(totals), encoding="utf-8")


def telemetry(name):
    return [json.loads(l) for l in (WORK / f"{name}_telemetry.jsonl").read_text(encoding="utf-8").splitlines()]


def snapshot(name, f):
    return Image.open(WORK / f"{name}_f{f:04d}.bmp").convert("RGB"), json.loads((WORK / f"{name}_f{f:04d}.json").read_text(encoding="utf-8"))


def events(name):
    data = (WORK / f"{name}_events.bin").read_bytes()
    out, i = {}, 0
    dt = np.dtype([("x", "<u2"), ("y", "<u2"), ("p", "i1"), ("c", "u1"), ("r", "<u2"), ("t", "<i8")])
    while i < len(data):
        f, n = struct.unpack_from("<II", data, i); i += 8
        out.setdefault(f, []).append(np.frombuffer(data, dtype=dt, count=n, offset=i)); i += 16 * n  # A picture's spikes come in several polls.
    return {f: np.concatenate(parts) for f, parts in out.items()}


if "extract" in STAGES: extract()
if "scenes" in STAGES: scenes()
if "run" in STAGES: run()
if "compose" in STAGES:
    import compose_showcase  # noqa: E402  (tools/showcase/compose_showcase.py: the layout)
    compose_showcase.compose(sys.modules[__name__])

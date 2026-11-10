"""Lays out what media_showcase wrote (tools/showcase/make_showcase.py runs it): the snapshots as the site's pictures, the event
camera's spikes as rasters, and build/showcase/showcase.json, the overlays and captions the pages draw over the snapshots (boxes,
the horizon, the region measured: from each snapshot's own sidecar and the telemetry). Comparisons with the scene's truth are
computed here and named as such."""
import json, math
import numpy as np
from PIL import Image


def webp(img, path, quality=86):
    img.save(path, "WEBP", quality=quality, method=6)


def compose(m):
    W, H, FPS, WORK, OUT = m.W, m.H, m.FPS, m.WORK, m.OUT
    truth = json.loads((WORK / "truth.json").read_text(encoding="utf-8"))
    out = {"source": "Big Buck Bunny (c) Blender Foundation, CC BY 3.0; the harbour is generated", "camera": [W, H], "fps": FPS}

    # 1. Shadow lift and neural reconstruction: two engines on the same frames, snapshots of the same picture.
    before, sb = m.snapshot("lift_off", 24)
    after, sa = m.snapshot("lift_on", 24)
    webp(before, OUT / "lift-before.webp"); webp(after, OUT / "lift-after.webp")
    before.save(WORK / "lift-before.png"); after.save(WORK / "lift-after.png")
    tb, ta = m.telemetry("lift_off")[24]["gpu"], m.telemetry("lift_on")[24]["gpu"]
    lum = lambda im: float(np.mean(np.asarray(im.convert("L"), np.float32)))
    out["lift"] = {"stops": truth["lift"]["stops"], "size": [after.width, after.height], "reconstruction": sa["picture"]["reconstruction"],
                   "shadow_lift": sa["picture"]["shadow_lift"], "gpu_ms": [tb["ms"], ta["ms"]], "mean_luma": [lum(before), lum(after)],
                   "hash": [sb["frame_hash"], sa["frame_hash"]]}

    # 2. Perception: the GPU image's snapshot with the sidecar's objects and horizon; the level run's snapshot beside it.
    f = 110
    shot, side = m.snapshot("perceive", f)
    level, lside = m.snapshot("perceive_level", f)
    webp(shot, OUT / "perceive.webp"); webp(level, OUT / "perceive-level.webp")
    shot.save(WORK / "perceive.png"); level.save(WORK / "perceive-level.png")
    tel = m.telemetry("perceive")
    ttc_truth = truth["perceive"]["ttc"]
    looming = [(i, r["ttc"], ttc_truth[i]) for i, r in enumerate(tel) if r["ttc"] > 0]
    errors = sorted(abs(a - b) / b for _, a, b in looming)
    # The horizon's line in the image: the generated scene's horizon (0.56 of the 432-sample scene, 36 cropped) through the centre,
    # turned by the tilt applied; drawn at the angle the engine measured.
    tilt, measured = truth["perceive"]["tilt_deg"], side["horizon"]["tilt_degrees"]
    sx, sy = shot.width / W, shot.height / H
    dy = 0.56 * 432 - 216
    px, py = -dy * math.sin(math.radians(tilt)), dy * math.cos(math.radians(tilt))
    cx, cy = (W / 2 + px) * sx, (H / 2 + py) * sy
    k = math.tan(math.radians(measured))
    objects = [{"id": o["track_id"], "box": [(o["x"] - o["width"] / 2) * shot.width, (o["y"] - o["height"] / 2) * shot.height, o["width"] * shot.width, o["height"] * shot.height],
                "approaching": o["approaching"], "ttc": o["ttc_seconds"], "v": [o["velocity_x"], o["velocity_y"]], "age": o["age"]} for o in side["objects"]]
    hazard = tel[f]["hazard"]
    out["perceive"] = {"frame": f, "size": [shot.width, shot.height], "tilt_deg": tilt, "horizon_deg": measured,
                       "flat_deg": m.telemetry("perceive_flat")[-1]["horizon_deg"], "level_deg": lside["orientation"]["level_degrees"],
                       "line": [0, cy - k * cx, shot.width, cy + k * (shot.width - cx)], "objects": objects, "hazard": [hazard[0] * shot.width, hazard[1] * shot.height],
                       "ttc": tel[f]["ttc"], "ttc_truth": ttc_truth[f], "looming_frames": len(looming), "frames": len(tel),
                       "first_looming_truth": looming[0][2] if looming else None, "ttc_median_error": errors[len(errors) // 2] if errors else None,
                       "meadow_deg": m.telemetry("meadow")[-1]["horizon_deg"], "meadow_tilt": truth["meadow"]["tilt_deg"]}

    # 3. The event camera: three pictures of spikes (the one before and after the snapshot's) as ON / OFF dots, both runs.
    window = (58, 59, 60)
    counts = {}
    for name in ("events_raw", "events_stab"):
        ev = m.events(name)
        tel = m.telemetry(name)
        canvas = np.zeros((H, W, 3), np.float32)
        for fr in window:
            e = ev.get(fr)
            if e is None:
                continue
            on = e["p"] > 0
            for sel, colour in ((on, (255, 138, 92)), (~on, (92, 178, 255))):
                np.add.at(canvas, (e["y"][sel], e["x"][sel]), np.array(colour, np.float32) * 0.55)
        glow = np.clip(canvas, 0, 255)
        img = Image.fromarray((np.dstack([glow[..., c] for c in range(3)]) + np.array([14, 14, 13])).clip(0, 255).astype(np.uint8)).resize((1280, 720), Image.NEAREST)
        webp(img, OUT / f"{name.replace('_', '-')}.webp", 90); img.save(WORK / f"{name.replace('_', '-')}.png")
        counts[name] = {"written": sum(r["events"] for r in tel), "window": int(sum(len(ev.get(fr, [])) for fr in window))}
    raw = np.frombuffer(m.WORK.joinpath("events.rgba").read_bytes(), np.uint8, count=W * H * 4, offset=W * H * 4 * 60).reshape(H, W, 4)[..., :3]
    cam = Image.fromarray(raw).resize((1280, 720), Image.LANCZOS)
    webp(cam, OUT / "events-input.webp"); cam.save(WORK / "events-input.png")
    out["events"] = {"frames": truth["events"]["frames"], "window": list(window), "counts": counts, "totals": json.loads((WORK / "events_totals.json").read_text(encoding="utf-8")),
                     "shake_px": [max(abs(s[0]) for s in truth["events"]["shake"]), max(abs(s[1]) for s in truth["events"]["shake"])]}

    # 4. Pulse through a shaking camera: the stabilised GPU image, the region vibrometry measured, the readings over time.
    shot, side = m.snapshot("pulse", 299)
    webp(shot, OUT / "pulse.webp"); shot.save(WORK / "pulse.png")
    tel = m.telemetry("pulse")
    last = tel[-1]
    reg = side["view"]["region"]
    scale = shot.width / (reg[2] - reg[0])
    l, t, r, b = last["vib_region"]
    shakes = truth["pulse"]["shake"]
    imu = [{"t": i / FPS, "truth": shakes[i][0] - shakes[i - 1][0], "virtual": row["ego"][0]} for i, row in enumerate(tel) if i > 0]
    rms = math.sqrt(sum((p["truth"] - p["virtual"]) ** 2 for p in imu[1:]) / (len(imu) - 1))
    chroma = m.telemetry("pulse_chroma")[-1]
    out["pulse"] = {"size": [shot.width, shot.height], "bpm_truth": truth["pulse"]["bpm"], "bpm": last["vital"][0], "confidence": last["vital"][1], "channel": last["vital"][2],
                    "chroma_bpm": chroma["vital"][0], "chroma_channel": chroma["vital"][2], "chroma_confidence": chroma["vital"][1], "seconds": last["vib_seconds"],
                    "region": [(l - reg[0]) * scale, (t - reg[1]) * scale, (r - l) * scale, (b - t) * scale],
                    "series": [{"t": round(i / FPS, 3), "bpm": row["vital"][0]} for i, row in enumerate(tel) if row.get("vital") and row["vital"][0] > 0],
                    "chroma_series": [{"t": round(i / FPS, 3), "bpm": row["vital"][0]} for i, row in enumerate(m.telemetry("pulse_chroma")) if row.get("vital") and row["vital"][0] > 0],
                    "imu": [{k: round(v, 4) for k, v in p.items()} for p in imu[:90]], "imu_rms": rms,
                    "shake_px": [max(abs(s[0]) for s in shakes), max(abs(s[1]) for s in shakes)], "amplitude": truth["pulse"]["pulse_rgb"]}
    (WORK / "showcase.json").write_text(json.dumps(out, indent=1), encoding="utf-8")
    print("composed", ", ".join(sorted(p.name for p in OUT.glob("*.webp"))))

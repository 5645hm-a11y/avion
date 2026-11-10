"""media_python_binding_test (API 16): the Python wheel as a user gets it.

    python -I tests/python_binding_test.py <avion.dll> <fixtures folder>

1. Builds the wheel (tools/python/build_wheel.py), checks its RECORD (every file's SHA-256 and size) and tag, unpacks it into a
   scratch site folder (what pip does for a py3-none-win_amd64 wheel) and imports `avion` from there only.
2. Live telemetry: host mode, the software bridge, playback_pulse.mp4 (a 72 BPM luma pulse on a patch at the centre): the rPPG
   reading reaches 72 +- 2 BPM; telemetry objects and the virtual IMU fields are readable.
3. Tensor export: 224 x 224 float32 of the fovea: shape (1, 3, 224, 224), values in [0, 1], exported twice into the same buffer
   (zero-copy: the __array_interface__ address does not change), float16 too.
4. Event camera on playback_two_targets.mp4 (two moving discs): ON and OFF events polled.
5. Bitstream inspection: playback_h264_bframes.mp4 (Media Foundation's H.264 encoder, B pictures, one shot) through decode_video
   into a BitstreamInspector: all 72 pictures inspected in decode order, I, P and B with their QP, the IDRs found, no false cut.
6. Shared textures (Windows): host mode with texture output decodes on the GPU ('hardware'), the textures' handles are given and a
   frame is acquired and released.
Exit 0 when every check passed."""
import ctypes, pathlib, shutil, subprocess, sys, tempfile, zipfile, hashlib, base64, time

failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print("FAIL:", what)
    return condition


def main():
    dll, fixtures = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve()
    root = pathlib.Path(__file__).resolve().parents[1]
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="avion_wheel_"))
    try:
        # 1. The wheel.
        built = subprocess.run([sys.executable, "-I", str(root / "tools" / "python" / "build_wheel.py"), str(dll), str(scratch / "dist")],
                               capture_output=True, text=True, check=False)
        check(built.returncode == 0, f"build_wheel: {built.stderr.strip()}")
        wheel = pathlib.Path(built.stdout.strip().splitlines()[-1])
        check(wheel.name == "avion-0.1.0-py3-none-win_amd64.whl", f"wheel name {wheel.name}")
        site = scratch / "site"
        with zipfile.ZipFile(wheel) as z:
            names = set(z.namelist())
            record = z.read("avion-0.1.0.dist-info/RECORD").decode("utf-8").splitlines()
            for line in record:
                name, hashed, size = line.rsplit(",", 2)
                if not hashed:
                    continue
                data = z.read(name)
                good = "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode() == hashed and int(size) == len(data)
                check(good, f"RECORD entry {name}")
            check({"avion/__init__.py", "avion/_capi.py", "avion/avion.dll", "avion/c_api.h", "avion-0.1.0.dist-info/LICENSING.md", "avion-0.1.0.dist-info/LICENSE"} <= names, "wheel contents")
            check("Tag: py3-none-win_amd64" in z.read("avion-0.1.0.dist-info/WHEEL").decode(), "wheel tag")
            z.extractall(site)
        sys.path.insert(0, str(site))
        import avion
        check(pathlib.Path(avion.__file__).resolve().is_relative_to(site.resolve()), "imported from the unpacked wheel")
        check(avion.api_version() == 17 and avion.__version__ == "0.1.0", f"API {avion.api_version()}, version {avion.__version__}")
        print(f"wheel {wheel.name}: {len(names)} files, {wheel.stat().st_size} bytes; avion {avion.__version__}, API {avion.api_version()}")

        # 2. Live telemetry: the pulse.
        best = None
        with avion.AvionEngine() as engine:
            engine.open(fixtures / "playback_pulse.mp4")
            check(engine.video_decoder() == "software", f"pulse decoder {engine.video_decoder()}")
            readings = []
            def each(e):
                t = e.telemetry()
                if t and t.vital_bpm:
                    readings.append(t)
            engine.play(seconds=11.0, virtual=True, interval=1 / 48, each=each)
            check(len(readings) >= 20, f"{len(readings)} pulse readings")
            if readings:
                best = readings[-1]
                check(abs(best.vital_bpm - 72.0) <= 2.0 and best.vital_confidence > 0.3, f"pulse {best.vital_bpm} BPM ({best.vital_confidence:.2f})")
                check(isinstance(best.objects, list) and set(best.virtual_imu) >= {"dx", "dy", "turn", "active"}, "telemetry fields")
                print(f"telemetry: {best}; rPPG {best.vital_bpm:.1f} BPM, confidence {best.vital_confidence:.2f}, window {best.vibrometry_seconds:.1f} s")

            # 3. Tensor export, zero-copy.
            x = engine.export_foveated_tensor(224, 224)
            interface = x.__array_interface__
            check(interface["shape"] == (1, 3, 224, 224) and interface["typestr"] == "<f4", f"tensor interface {interface}")
            view = x.data
            values = view.cast("B").cast("f")
            low, high = min(values), max(values)
            check(0.0 <= low <= high <= 1.0 and high > low, f"tensor values {low}..{high}")
            address = interface["data"][0]
            engine.step(virtual=True, interval=1 / 24)
            y = engine.export_foveated_tensor(224, 224)
            check(y is x and y.__array_interface__["data"][0] == address, "the same buffer for the next export (no copy, no new buffer)")
            h = engine.export_foveated_tensor(112, 112, dtype="float16", normalize="imagenet")
            check(h.__array_interface__["typestr"] == "<f2" and h.nbytes == 3 * 112 * 112 * 2, "float16 tensor")
            print(f"tensor: {interface['shape']} float32 at {address:#x}, values {low:.3f}..{high:.3f}; float16 {h.shape}")

        # 4. The event camera.
        with avion.AvionEngine() as engine:
            engine.open(fixtures / "playback_two_targets.mp4")
            engine.play(seconds=0.5, virtual=True, interval=1 / 30)
            check(engine.set_event_camera(True), "event camera on")
            on = off = total = 0
            for _ in range(30):
                engine.step(virtual=True, interval=1 / 30)
                spikes = engine.poll_event_spikes()
                total += len(spikes)
                for sx, sy, polarity, contrast, stamp in spikes:
                    on += polarity > 0; off += polarity < 0
            state = engine.event_camera()
            check(total > 100 and on > 0 and off > 0, f"events: {total} ({on} ON, {off} OFF)")
            print(f"event camera: {total} events in a second ({on} ON, {off} OFF), sensor {state['width']}x{state['height']}")

        # 5. Bitstream inspection through decode_video.
        results = []
        inspector = {}
        def decode(unit):
            if "i" not in inspector:
                inspector["i"] = avion.BitstreamInspector(unit.codec, ctypes.string_at(unit.config, unit.config_bytes), unit.nal_length_bytes)
            info = inspector["i"].inspect(unit.data, unit.bytes)
            if info:
                results.append((unit.pts_100ns / 1e7, info))
            return False  # Not decoded by this host: the engine goes on (no picture planes).
        with avion.AvionEngine(decode_video=decode) as engine:
            engine.open(fixtures / "playback_h264_bframes.mp4")
            check(engine.video_decoder() == "host", f"bframes decoder {engine.video_decoder()}")
            engine.play(virtual=True, interval=1 / 50)
        types = {info["picture_type"] for _, info in results}
        keys = [t for t, info in results if info["key"]]
        cuts = [t for t, info in results if info["scene_cut_probability"] >= 0.5]
        reordered = any(results[i + 1][0] < results[i][0] for i in range(len(results) - 1)) # Decode order: B after its later P.
        check(len(results) == 72 and types == {"I", "P", "B"} and all(info["qp"] >= 0 for _, info in results), f"{len(results)} pictures, types {types}")
        check(len(keys) >= 2 and reordered and not cuts, f"keys at {keys}, reordered {reordered}, cuts {cuts}") # One shot: no cut.
        print(f"bitstream: {len(results)} pictures of a Media Foundation H.264 stream inspected (types {sorted(types)}, IDRs at "
              f"{', '.join(f'{k:.2f}' for k in keys)} s, QP {results[0][1]['qp']}), no false scene cut")

        # 6. Shared textures from host mode's GPU decoding (Windows).
        if sys.platform == "win32":
            with avion.AvionEngine(texture=(320, 180), output_format=avion.OUTPUT_FORMAT_PQ10) as engine:
                engine.open(fixtures / "playback_chapters.mp4")
                decoder = engine.video_decoder()
                check(decoder == "hardware", f"texture decoder {decoder}")
                textures = frame = None
                began = time.perf_counter()
                while time.perf_counter() - began < 5 and not frame:
                    engine.step()
                    textures = textures or engine.output_textures()
                    frame = engine.acquire_output_frame() if textures else None
                check(textures and all(textures["textures"]) and textures["dxgi_format"] == 24 and textures["color_space"] == 12, f"textures {textures}")
                check(frame is not None and frame["fence_value"] > 0, f"frame {frame}")
                if frame:
                    engine.release_output_frame(frame["index"], 0)
                    print(f"textures: {textures['width']}x{textures['height']} DXGI {textures['dxgi_format']}, colour space {textures['color_space']}, "
                          f"frame {frame['sequence']} at {frame['pts']:.3f} s, decoder {decoder}")
    finally:
        sys.path[:] = [p for p in sys.path if not p.startswith(str(scratch))]
        shutil.rmtree(scratch, ignore_errors=True)
    print("python binding: " + (f"{failures} FAILED" if failures else "all checks passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

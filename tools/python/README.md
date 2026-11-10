# AVION for Python

A zero-dependency `ctypes` binding of `avion.dll`, the AVION media engine's C99 SDK (API 14). Windows x64, Python 3.8 or later.

```
pip install avion-0.1.0-py3-none-win_amd64.whl
```

```python
import avion

with avion.AvionEngine() as engine:                  # Host mode: your thread steps the engine.
    engine.open("clip.mp4")
    while engine.step():                             # Real time; step(virtual=True) runs as fast as it decodes.
        t = engine.telemetry()                       # Perception of the picture just shown, or None.
        if t and t.vital_bpm:
            print(f"{t.pts_100ns / 1e7:.2f} s  pulse {t.vital_bpm:.0f} BPM, vibration {t.vibration_hz} Hz, TTC {t.ttc_seconds}")
        x = engine.export_foveated_tensor(224, 224)  # 1 x 3 x 224 x 224 float32, written in place by the engine.
        # numpy.asarray(x) and torch.from_numpy(numpy.asarray(x)) are views of the same buffer: no copy.
```

- `telemetry()`: remote photoplethysmography (`vital_bpm`), vibration (`vibration_hz`), time to contact (`ttc_seconds`), the
  four-object table (`objects`), the virtual IMU (`virtual_imu`).
- `export_foveated_tensor(width, height, channels=3, dtype="float32"|"float16", normalize="unit"|"imagenet"|"signed",
  region="fovea"|"view"|"full", box=(l, t, r, b))`: a `Tensor` with the buffer protocol and `__array_interface__`.
- `set_event_camera()`, `poll_event_spikes()`: the event camera's ON/OFF events (NumPy structured view).
- `avion.BitstreamInspector(codec, config)`: bitstream perception (picture type, QP, cost, scene-cut probability) without decoding.
  Pass `decode_video=callable` to `AvionEngine` to see every compressed picture of a file.
- `AvionEngine(texture=(w, h), output_format=avion.OUTPUT_FORMAT_PQ10)`: hardware decoding on the GPU into three shared Direct3D
  textures (`output_textures()`, `acquire_output_frame()`, `release_output_frame()`), 8-bit, 10-bit PQ or FP16 scRGB.

Dual-licensed: the GNU AGPLv3 (`LICENSE`) or a commercial licence from the author for closed-source software, devices and services (`LICENSING.md`). Source: https://github.com/5645hm-a11y/avion

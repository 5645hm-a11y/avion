"""AVION for Python: a zero-dependency ctypes binding of avion.dll (the C99 SDK, API 16).

    import avion
    with avion.AvionEngine() as engine:            # Host mode: this thread steps the engine.
        engine.open("clip.mp4")
        while engine.step():                       # Real time by default (virtual=True: as fast as it decodes).
            t = engine.telemetry()                 # rPPG BPM, vibration Hz, TTC, the 4-target table, the virtual IMU.
            x = engine.export_foveated_tensor()    # 1 x 3 x 224 x 224 float32, zero-copy: numpy.asarray(x), torch.from_numpy(...).

Nothing here copies a picture: tensors are written by the engine straight into a buffer this binding owns (one per shape, reused),
exposed through the buffer protocol (memoryview) and NumPy's __array_interface__. The engine itself allocates nothing after open.
Licence: GNU AGPLv3 (LICENSE) or a commercial licence from the author for closed-source software, devices and services
(LICENSING.md). Source: https://github.com/5645hm-a11y/avion
"""
import ctypes
import os
import time

from . import _capi
from ._capi import (CODEC_H264, CODEC_HEVC, DECODER_HARDWARE, DECODER_HOST, DECODER_NONE, DECODER_SOFTWARE, MODULE_ALL, MODULE_HUD,
                    MODULE_TELEMETRY, MODULE_VOICE, OUTPUT_FORMAT_PQ10, OUTPUT_FORMAT_RGBA8, OUTPUT_FORMAT_SCRGB16, TENSOR_F16, TENSOR_F32)

__version__ = "0.1.0"
__all__ = ["AvionEngine", "BitstreamInspector", "Tensor", "EventSpikes", "AvionError", "api_version", "inspect_bitstream", "probe", "__version__"]

_lib = None


def library():
    """The loaded avion.dll (loaded once, from this package's folder or AVION_DLL)."""
    global _lib
    if _lib is None:
        _lib = _capi.load()
    return _lib


def api_version():
    return int(library().media_api_version())


class AvionError(RuntimeError):
    def __init__(self, call, status, stage=0, text=""):
        self.call, self.status, self.stage, self.text = call, status, stage, text
        super().__init__(f"{call}: {_capi.STATUS_NAMES.get(status, status)}" + (f" ({text})" if text else f" (stage {stage})" if stage else ""))


def probe(path):
    """What a file holds and whether AVION plays it, without an engine (API 15): a dict with container, playable, duration (s),
    reason and tracks (kind, codec, description, supported, reason, ...). Raises AvionError when the file cannot be read."""
    p = _capi.Probe(); p.struct_size = ctypes.sizeof(p)
    status = library().media_probe(str(path).encode("utf-8"), ctypes.byref(p))
    if status not in (_capi.OK,):
        raise AvionError("media_probe", status, text=p.reason.decode("utf-8", "replace"))
    text = lambda b: b.decode("utf-8", "replace")
    tracks = [{"id": t.id, "kind": _capi.TRACK_KINDS.get(t.kind, "other"), "codec": text(t.codec), "description": text(t.description),
               "supported": bool(t.supported), "width": t.width, "height": t.height, "channels": t.channels, "sample_rate": t.sample_rate,
               "duration": t.duration_100ns / 1e7, "language": text(t.language), "reason": text(t.reason)} for t in p.tracks[:p.track_count]]
    return {"container": _capi.CONTAINERS.get(p.container, "unknown"), "playable": bool(p.playable), "duration": p.duration_100ns / 1e7,
            "reason": text(p.reason), "tracks": tracks}


def _check(call, status, allowed=(_capi.OK,)):
    if status not in allowed:
        raise AvionError(call, status)
    return status


class Tensor:
    """A model's input, NCHW, written by the engine into this object's own buffer (no copy on the way out).

    numpy.asarray(tensor) is a view of that buffer (__array_interface__); memoryview(tensor.data) too. The next export of the same
    shape writes into the same buffer: keep a copy if you need the previous one."""

    def __init__(self, shape, dtype):
        self.shape, self.dtype = tuple(shape), dtype
        count = 1
        for n in self.shape:
            count *= n
        self.nbytes = count * (4 if dtype == "float32" else 2)
        self._buffer = (ctypes.c_uint8 * self.nbytes)()
        self.pts_100ns, self.region, self.target_track_id, self.flags = -1, (0.0, 0.0, 0.0, 0.0), 0, 0

    @property
    def address(self):
        return ctypes.addressof(self._buffer)

    @property
    def data(self):
        """A memoryview of the elements, shaped as the tensor (format 'f' or 'e')."""
        return memoryview(self._buffer).cast("B").cast("f" if self.dtype == "float32" else "e", self.shape)

    @property
    def __array_interface__(self):
        return {"shape": self.shape, "typestr": "<f4" if self.dtype == "float32" else "<f2", "data": (self.address, False), "version": 3}

    def values(self):
        """The elements as a flat list (convenience; this one copies)."""
        return list(self.data.cast("B").cast("f" if self.dtype == "float32" else "e"))


class EventSpikes:
    """Event-camera events polled from the engine's ring: len(), iteration (x, y, polarity, contrast, timestamp_us), and a NumPy
    structured view through __array_interface__ (fields x, y, polarity, contrast, reserved, timestamp_us)."""

    DESCR = [("x", "<u2"), ("y", "<u2"), ("polarity", "|i1"), ("contrast", "|u1"), ("reserved", "<u2"), ("timestamp_us", "<i8")]

    def __init__(self, capacity):
        self._array = (_capi.EventSpike * capacity)()
        self.capacity, self.count = capacity, 0

    def __len__(self):
        return self.count

    def __iter__(self):
        for i in range(self.count):
            s = self._array[i]
            yield (s.x, s.y, s.polarity, s.contrast, s.timestamp_us)

    @property
    def __array_interface__(self):
        return {"shape": (self.count,), "typestr": "|V16", "descr": self.DESCR, "data": (ctypes.addressof(self._array), False), "version": 3}


class BitstreamInspector:
    """Bitstream perception (API 9): what a compressed H.264 / HEVC picture says before anything is decoded (type, QP, cost, new
    detail, bursts, the probability of a scene cut). Under a microsecond a picture; no engine needed."""

    def __init__(self, codec=CODEC_H264, config=b"", nal_length_bytes=0):
        self._storage = _capi.BitstreamInspectorStorage()
        self._config = (ctypes.c_uint8 * max(len(config), 1)).from_buffer_copy(bytes(config) or b"\0")
        status = library().media_bitstream_inspector_init(ctypes.byref(self._storage), codec, self._config if config else None, len(config), nal_length_bytes)
        _check("media_bitstream_inspector_init", status)

    def inspect(self, data, size=None):
        """One access unit as stored (length-prefixed NAL units): a dict, or None when it holds no picture (parameter sets, SEI).
        `data`: bytes-like, or a ctypes pointer with `size`."""
        info = _capi.BitstreamInfo(); info.struct_size = ctypes.sizeof(info)
        if size is None:
            raw = bytes(data); size = len(raw)
            data = (ctypes.c_uint8 * max(size, 1)).from_buffer_copy(raw or b"\0")
        status = library().media_inspect_bitstream_packet(ctypes.byref(self._storage), ctypes.cast(data, ctypes.POINTER(ctypes.c_uint8)), size, ctypes.byref(info))
        if status == _capi.EMPTY:
            return None
        _check("media_inspect_bitstream_packet", status)
        return {"index": info.index, "bytes": info.bytes, "slices": info.slices, "picture_type": "-IPB"[info.picture_type] if info.picture_type < 4 else "?",
                "key": bool(info.flags & _capi.BITSTREAM_KEY), "qp": info.qp, "complexity": info.complexity, "activity": info.activity, "burst": info.burst,
                "scene_cut_probability": info.scene_cut_probability, "scene_cut": bool(info.flags & _capi.BITSTREAM_SCENE_CUT),
                "motion_burst": bool(info.flags & _capi.BITSTREAM_MOTION_BURST), "width": info.width, "height": info.height,
                "revised_index": info.revised_index if info.flags & _capi.BITSTREAM_REVISION else None,
                "revised_scene_cut_probability": info.revised_scene_cut_probability if info.flags & _capi.BITSTREAM_REVISION else None}


def inspect_bitstream(units, codec=CODEC_H264, config=b"", nal_length_bytes=0):
    """Every unit of an iterable of access units through one inspector: the list of their results (None for units without a picture)."""
    inspector = BitstreamInspector(codec, config, nal_length_bytes)
    return [inspector.inspect(u) for u in units]


class Telemetry:
    """media_frame_telemetry_t, readable: perception, remote vibrometry (vibration Hz, rPPG BPM), the 4-target object table and the
    virtual IMU of the newest picture. `raw` is the C struct."""

    def __init__(self, raw):
        self.raw = raw
        f = raw.flags
        self.pts_100ns, self.sequence = raw.pts_100ns, raw.sequence
        self.motion = (raw.motion_x, raw.motion_y)
        self.ttc_seconds = raw.ttc_seconds if f & _capi.TELEMETRY_LOOMING else None
        self.vibration_hz = raw.vibration_hz if f & _capi.TELEMETRY_VIBRATION else None
        self.vibrometry_confidence = raw.vibrometry_confidence
        self.vital_bpm = raw.vital_bpm if f & _capi.TELEMETRY_VITALS else None
        self.vital_confidence = raw.vital_confidence
        self.vibrometry_seconds = raw.vibrometry_seconds
        self.objects = [{"track_id": o.track_id, "x": o.x, "y": o.y, "width": o.width, "height": o.height, "velocity": (o.velocity_x, o.velocity_y),
                         "ttc_seconds": o.ttc_seconds if o.flags & 1 else None, "saliency": o.saliency, "age": o.age, "target": bool(o.flags & 4)}
                        for o in raw.objects[:raw.object_count]]
        self.target_track_id = raw.target_track_id
        self.virtual_imu = {"dx": raw.ego_dx, "dy": raw.ego_dy, "turn": raw.ego_turn, "tiles": raw.ego_tiles, "inliers": raw.ego_inliers,
                            "active": bool(f & _capi.TELEMETRY_VIRTUAL_IMU)}
        self.view = (raw.view_zoom, raw.view_x, raw.view_y)
        self.flags = f

    def __repr__(self):
        return (f"Telemetry(pts={self.pts_100ns / 1e7:.3f}s, bpm={self.vital_bpm}, vibration_hz={self.vibration_hz}, ttc={self.ttc_seconds}, "
                f"objects={len(self.objects)}, virtual_imu={self.virtual_imu['active']})")


_DECODERS = {DECODER_NONE: "none", DECODER_SOFTWARE: "software", DECODER_HOST: "host", DECODER_HARDWARE: "hardware"}
_DTYPES = {"float32": TENSOR_F32, "float16": TENSOR_F16}
_NORMALIZE = {"unit": 0, "imagenet": 1, "signed": 2}
_REGIONS = {"fovea": 0, "view": 1, "full": 2, "region": 3}


class AvionEngine:
    """One engine in host mode (MEDIA_PLAYBACK_HOST): this thread opens, steps, queries and closes it; the engine starts no thread.

    modules: MEDIA_MODULE_* flags (default: everything but voice and the dock). texture=(width, height): texture output; on Windows
    the engine then decodes H.264 / HEVC on the GPU into three shared Direct3D textures (output_textures(), acquire_output_frame()),
    in output_format (OUTPUT_FORMAT_RGBA8, _PQ10, _SCRGB16). decode_video: a callable(unit) -> bool for the host's own decoder (it
    sees every compressed picture: e.g. a BitstreamInspector), True when it decoded it."""

    def __init__(self, modules=MODULE_ALL & ~(MODULE_VOICE | MODULE_HUD), audio_rate=48000, audio_channels=2, texture=None,
                 output_format=OUTPUT_FORMAT_RGBA8, decode_video=None, on_present=None):
        lib = library()
        self._config = _capi.Config(); lib.media_engine_config_init(ctypes.byref(self._config))
        c = self._config
        c.modules, c.playback, c.audio_rate, c.audio_channels = modules, _capi.PLAYBACK_HOST, audio_rate, audio_channels
        if texture:
            c.output_flags, c.output_width, c.output_height, c.output_format = _capi.OUTPUT_TEXTURE, texture[0], texture[1], output_format
        self._callbacks = []
        if decode_video:
            def decode(_context, unit):
                try:
                    return 0 if decode_video(unit.contents) else 1
                except Exception:  # Never across the C boundary.
                    return 1
            self._decode = _capi.DecodeVideoCallback(decode); c.decode_video = ctypes.cast(self._decode, ctypes.c_void_p)
        if on_present:
            def present(_context, picture):
                try:
                    on_present(picture.contents)
                except Exception:
                    pass
            self._present = _capi.PresentCallback(present); c.on_present = ctypes.cast(self._present, ctypes.c_void_p)
        size, align = lib.media_engine_storage_bytes(), lib.media_engine_storage_alignment()
        self._storage = (ctypes.c_uint8 * (size + align))()
        base = (ctypes.addressof(self._storage) + align - 1) // align * align
        self._handle = ctypes.c_void_p()
        _check("media_engine_create", lib.media_engine_create(ctypes.byref(c), base, size, ctypes.byref(self._handle)))
        self._audio = (ctypes.c_float * (480 * max(audio_channels, 1)))()
        self._tensors, self._spikes, self._opened = {}, None, False
        self._clock0 = self._virtual = None
        self._audio_due = 0

    # ---- Lifecycle ----
    def open(self, path):
        lib = library()
        status = lib.media_engine_open(self._handle, str(path).encode("utf-8"))
        if status != _capi.OK:
            why = ctypes.create_string_buffer(512); lib.media_engine_last_error_text(self._handle, why, 512)
            raise AvionError("media_engine_open", status, lib.media_engine_last_error(self._handle, None), why.value.decode("utf-8", "replace"))
        self._opened, self._clock0, self._virtual, self._audio_due = True, time.perf_counter(), 0, 0
        return self

    def step(self, now_100ns=None, virtual=False, interval=0.01):
        """One pass (demux, decode, the clock, presentation, commands) and the audio due since the last one. Real time by default
        (sleeps `interval`); virtual=True advances a virtual clock by `interval` per step instead (as fast as it decodes). False at
        the end of the media."""
        lib = library()
        if now_100ns is None:
            if virtual:
                self._virtual += int(interval * 1e7); now_100ns = self._virtual
            else:
                time.sleep(interval); now_100ns = int((time.perf_counter() - self._clock0) * 1e7)
        status = lib.media_engine_step(self._handle, now_100ns)
        if status == _capi.EMPTY:
            return False
        _check("media_engine_step", status)
        while self._config.audio_channels and self._audio_due <= now_100ns:  # 10 ms of audio per 10 ms (the host's audio callback).
            lib.media_engine_pump_audio(self._handle, self._audio, 480, self._audio_due); self._audio_due += 100000
        return True

    def play(self, seconds=None, virtual=True, interval=0.01, each=None):
        """Steps until the end (or `seconds` of the clock); `each(engine)` after every step."""
        steps = 0
        while seconds is None or steps * interval < seconds:
            if not self.step(virtual=virtual, interval=interval):
                return False
            steps += 1
            if each:
                each(self)
        return True

    def close(self):
        if self._handle and self._opened:
            library().media_engine_close(self._handle); self._opened = False

    def destroy(self):
        if self._handle:
            self.close(); library().media_engine_destroy(self._handle); self._handle = ctypes.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.destroy()

    def __del__(self):
        try:
            self.destroy()
        except Exception:
            pass

    # ---- Commands and state ----
    def command(self, text):
        """A command in any of the nine languages ("pause", "go to 1:42", "track target", "חשוך"): True when it ran."""
        return library().media_engine_command(self._handle, text.encode("utf-8")) == _capi.OK

    def control(self, name, value=1.0, command_id=0):
        """A structured control (API 15): "seek" (seconds), "seek_relative", "pause" (1/0), "volume" (0..1), "mute" (1/0),
        "speed" (0.25..4), "audio_track"/"subtitle_track" (an id; both modes since API 17). Volume above 1 is a boost to x2 through a soft
        limiter. Queued; the next step runs it. ValueError for an unknown name, AvionError for a value out of range."""
        if name not in _capi.CONTROLS:
            raise ValueError(f"unknown control {name!r}: one of {', '.join(_capi.CONTROLS)}")
        _check("media_engine_post_control", library().media_engine_post_control(self._handle, _capi.CONTROLS[name], float(value), command_id))

    # ---- Tracks, subtitles after open, snapshots into memory (API 17) ----
    def tracks(self):
        """Every track of the file and the subtitle sidecar (id 0), both modes: dicts of id, kind, codec, language, title, channels,
        rate, width, height, selectable, active, default."""
        count = ctypes.c_uint32(0)
        _check("media_engine_track_count", library().media_engine_track_count(self._handle, ctypes.byref(count)))
        kinds = {1: "video", 2: "audio", 3: "subtitle"}
        out = []
        for i in range(count.value):
            t = _capi.TrackInfo(); t.struct_size = ctypes.sizeof(t)
            _check("media_engine_track_info", library().media_engine_track_info(self._handle, i, ctypes.byref(t)))
            text = lambda b: b.split(b"\0", 1)[0].decode("utf-8", "replace")
            out.append({"id": t.id, "kind": kinds.get(t.kind, "other"), "codec": text(t.codec), "language": text(t.language), "title": text(t.title),
                        "channels": t.channels, "rate": t.sample_rate, "width": t.width, "height": t.height, "selectable": bool(t.selectable),
                        "active": bool(t.active), "default": bool(t.is_default)})
        return out

    def attach_subtitles(self, path):
        """A .srt/.vtt file shown from now on as the sidecar (track id 0), the file's own tracks kept (host mode; the windowed engine
        answers AvionError MEDIA_ERROR_UNSUPPORTED in this build)."""
        _check("media_engine_attach_subtitles", library().media_engine_attach_subtitles(self._handle, os.fsencode(path) if os.name != "nt" else str(path).encode("utf-8")))

    def snapshot_to_memory(self, pixels="bgra8"):
        """The snapshot media_engine_save_snapshot would write, as (width, height, bytes) of 8-bit pixels ("bgra8" or "rgba8"),
        rows width x 4 bytes, alpha 255; and its frame hash. Synchronous in both modes."""
        b = _capi.SnapshotBuffer(); b.struct_size = ctypes.sizeof(b); b.format = _capi.PIXELS[pixels]
        probe = ctypes.create_string_buffer(16)
        b.pixels = ctypes.cast(probe, ctypes.c_void_p); b.capacity_bytes = 16
        status = library().media_engine_snapshot_to_memory(self._handle, ctypes.byref(b))
        if status == _capi.OK:
            return b.width, b.height, probe.raw[:b.width * b.height * 4], b.frame_hash
        if not b.width:
            _check("media_engine_snapshot_to_memory", status)
        buffer = ctypes.create_string_buffer(b.width * b.height * 4)
        b.pixels = ctypes.cast(buffer, ctypes.c_void_p); b.capacity_bytes = len(buffer); b.stride = 0
        _check("media_engine_snapshot_to_memory", library().media_engine_snapshot_to_memory(self._handle, ctypes.byref(b)))
        return b.width, b.height, buffer.raw[:b.width * b.height * 4], b.frame_hash

    def state(self):
        s = _capi.PlaybackState(); _check("media_engine_state", library().media_engine_state(self._handle, ctypes.byref(s)))
        return {"position": s.position_100ns / 1e7, "duration": s.duration_100ns / 1e7, "paused": bool(s.paused), "muted": bool(s.muted),
                "volume": s.volume, "speed": s.speed, "presented": s.presented, "dropped": s.dropped, "tracking": s.tracking}

    def stats(self):
        s = _capi.Stats(); _check("media_engine_stats", library().media_engine_stats(self._handle, ctypes.byref(s)))
        return {name: getattr(s, name) for name, _ in _capi.Stats._fields_}

    def video_decoder(self):
        """'hardware' (the GPU), 'host' (decode_video), 'software' (the engine's bridge) or 'none'."""
        return _DECODERS.get(library().media_query_video_decoder(self._handle), "none")

    # ---- Perception ----
    def telemetry(self):
        """The newest picture's telemetry (None before the first measured picture)."""
        t = _capi.FrameTelemetry(); t.struct_size = ctypes.sizeof(t)
        status = library().media_query_frame_telemetry(self._handle, ctypes.byref(t))
        if status == _capi.EMPTY:
            return None
        _check("media_query_frame_telemetry", status)
        return Telemetry(t)

    def export_foveated_tensor(self, width=224, height=224, channels=3, dtype="float32", normalize="unit", region="fovea", bgr=False, box=None):
        """The region of the picture shown last as NCHW (1 x channels x height x width): fovea (the tracked target or the view's
        steadied fovea), view, full, or region with box=(left, top, right, bottom) fractions. Zero-copy: the engine writes into the
        returned Tensor's buffer (reused for the next export of the same shape)."""
        r = _capi.TensorRequest(); library().media_tensor_request_init(ctypes.byref(r))
        r.width, r.height, r.channels, r.format = width, height, channels, _DTYPES[dtype]
        r.normalize, r.region, r.flags = _NORMALIZE[normalize], _REGIONS[box and "region" or region], 1 if bgr else 0
        if box:
            r.left, r.top, r.right, r.bottom = box
        key = (width, height, channels, dtype)
        tensor = self._tensors.get(key)
        if tensor is None:
            tensor = self._tensors[key] = Tensor((1, channels, height, width), dtype)
        info = _capi.TensorInfo(); info.struct_size = ctypes.sizeof(info)
        _check("media_export_foveated_tensor", library().media_export_foveated_tensor(self._handle, ctypes.byref(r), tensor.address, tensor.nbytes, ctypes.byref(info)))
        tensor.pts_100ns, tensor.region, tensor.target_track_id, tensor.flags = info.pts_100ns, (info.left, info.top, info.right, info.bottom), info.target_track_id, info.flags
        return tensor

    def set_event_camera(self, on=True, contrast_threshold=0.15, stabilized=True):
        if not on:
            return _check("media_engine_set_event_camera", library().media_engine_set_event_camera(self._handle, None)) == _capi.OK
        c = _capi.EventCameraConfig(); library().media_event_camera_config_init(ctypes.byref(c))
        c.contrast_threshold, c.flags = contrast_threshold, 0 if stabilized else 1
        return _check("media_engine_set_event_camera", library().media_engine_set_event_camera(self._handle, ctypes.byref(c))) == _capi.OK

    def poll_event_spikes(self, capacity=65536):
        """The oldest events (at most `capacity`), in time order, in a reused EventSpikes (empty when there are none)."""
        if self._spikes is None or self._spikes.capacity != capacity:
            self._spikes = EventSpikes(capacity)
        count = ctypes.c_uint32()
        status = library().media_poll_event_spikes(self._handle, self._spikes._array, capacity, ctypes.byref(count))
        _check("media_poll_event_spikes", status, (_capi.OK, _capi.EMPTY))
        self._spikes.count = count.value if status == _capi.OK else 0
        return self._spikes

    def event_camera(self):
        s = _capi.EventCameraState(); s.struct_size = ctypes.sizeof(s)
        _check("media_query_event_camera", library().media_query_event_camera(self._handle, ctypes.byref(s)))
        return {name: getattr(s, name) for name, _ in _capi.EventCameraState._fields_ if name != "struct_size"}

    # ---- Texture output (Windows; API 13, 14) ----
    def output_textures(self):
        """The shared textures' NT handles and fences, for a renderer of the host's own (open them with ID3D12Device::OpenSharedHandle
        or ID3D11Device1::OpenSharedResource1 on the adapter the LUID names). None while none is ready."""
        t = _capi.OutputTextures(); t.struct_size = ctypes.sizeof(t)
        status = library().media_engine_query_output_textures(self._handle, ctypes.byref(t))
        if status == _capi.EMPTY:
            return None
        _check("media_engine_query_output_textures", status)
        return {"width": t.width, "height": t.height, "dxgi_format": t.format, "generation": t.generation, "textures": [t.textures[i] for i in range(3)],
                "fence": t.fence, "host_fence": t.host_fence, "adapter_luid": (t.adapter_luid_high << 32) | t.adapter_luid_low,
                "color_space": t.color_space, "sdr_white_nits": t.sdr_white_nits}

    def acquire_output_frame(self):
        f = _capi.OutputFrame(); f.struct_size = ctypes.sizeof(f)
        status = library().media_engine_acquire_output_frame(self._handle, ctypes.byref(f))
        if status == _capi.EMPTY:
            return None
        _check("media_engine_acquire_output_frame", status)
        return {"index": f.index, "fence_value": f.fence_value, "pts": f.pts_100ns / 1e7, "generation": f.generation, "sequence": f.sequence, "transfer": f.transfer}

    def release_output_frame(self, index, host_fence_value=0):
        _check("media_engine_release_output_frame", library().media_engine_release_output_frame(self._handle, index, host_fence_value))

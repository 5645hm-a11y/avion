"""ctypes declarations of avion.dll's C99 SDK (include/media/c_api.h, API 16), x64 layouts.

Every struct here is laid out by ctypes with the platform's natural alignment, field for field as the header declares it; the
sizes are asserted below against the ones the C tests assert (tests/c_api_tests.c, tests/embed_tests.c, src/sdk/CApi.cpp).
Nothing in this module allocates per frame on the engine's side: the engine writes into buffers the caller owns.
"""
import ctypes
import os
import pathlib

c_u8, c_i8, c_u16, c_u32, c_i32, c_u64, c_i64, c_f32 = (ctypes.c_uint8, ctypes.c_int8, ctypes.c_uint16, ctypes.c_uint32, ctypes.c_int32,
                                                        ctypes.c_uint64, ctypes.c_int64, ctypes.c_float)
c_ptr = ctypes.c_void_p

API_VERSION = 17

# media_status
OK, INVALID_ARGUMENT, STORAGE, STATE, UNSUPPORTED, OPEN, PLAYBACK, REFUSED, EMPTY = range(9)
STATUS_NAMES = {0: "MEDIA_OK", 1: "MEDIA_ERROR_INVALID_ARGUMENT", 2: "MEDIA_ERROR_STORAGE", 3: "MEDIA_ERROR_STATE", 4: "MEDIA_ERROR_UNSUPPORTED",
                5: "MEDIA_ERROR_OPEN", 6: "MEDIA_ERROR_PLAYBACK", 7: "MEDIA_ERROR_REFUSED", 8: "MEDIA_ERROR_EMPTY"}
# media_module_flags
MODULE_VOICE, MODULE_HUD, MODULE_SUBTITLES, MODULE_NEURAL_UPSCALE, MODULE_CHAPTERS, MODULE_TELEMETRY = 1, 2, 4, 8, 16, 32
MODULE_ALL = 63
PLAYBACK_WINDOW, PLAYBACK_HOST = 0, 1
CODEC_H264, CODEC_HEVC = 1, 2
OUTPUT_TEXTURE = 1
OUTPUT_FORMAT_RGBA8, OUTPUT_FORMAT_PQ10, OUTPUT_FORMAT_SCRGB16 = 0, 1, 2
DECODER_NONE, DECODER_SOFTWARE, DECODER_HOST, DECODER_HARDWARE = 0, 1, 2, 3
PICTURE_DECODED, PICTURE_HOST, PICTURE_LANDING, PICTURE_TRACKING, PICTURE_TEXTURE = 1, 2, 4, 8, 16
TENSOR_F32, TENSOR_F16 = 1, 2
TENSOR_UNIT, TENSOR_IMAGENET, TENSOR_SIGNED = 0, 1, 2
TENSOR_FOVEA, TENSOR_VIEW, TENSOR_FULL, TENSOR_REGION = 0, 1, 2, 3
TENSOR_BGR = 1
TELEMETRY_FLOW, TELEMETRY_LOOMING, TELEMETRY_EGO, TELEMETRY_SOUND, TELEMETRY_SACCADE, TELEMETRY_STABILIZED = 1, 2, 4, 8, 16, 32
TELEMETRY_GROUND, TELEMETRY_FOVEA, TELEMETRY_HORIZON, TELEMETRY_BARS, TELEMETRY_OBJECTS = 64, 128, 256, 512, 1024
TELEMETRY_VIBRATION, TELEMETRY_VITALS, TELEMETRY_VIRTUAL_IMU = 2048, 4096, 8192
BITSTREAM_KEY, BITSTREAM_PARAMETER_SETS, BITSTREAM_SCENE_CUT, BITSTREAM_MOTION_BURST, BITSTREAM_QP, BITSTREAM_REVISION = 1, 2, 4, 8, 16, 32

FrameCallback = ctypes.CFUNCTYPE(None, c_ptr, c_ptr)
SpeakCallback = ctypes.CFUNCTYPE(ctypes.c_size_t, c_ptr, ctypes.c_char_p, c_u32, c_i32, ctypes.POINTER(c_f32), ctypes.c_size_t)


class VideoUnit(ctypes.Structure):
    """media_video_unit_t: one compressed picture, in decode order (host mode's decode_video)."""
    _fields_ = [("data", ctypes.POINTER(c_u8)), ("bytes", c_u32), ("nal_length_bytes", c_u32), ("config", ctypes.POINTER(c_u8)),
                ("config_bytes", c_u32), ("codec", c_u32), ("pts_100ns", c_i64), ("dts_100ns", c_i64), ("duration_100ns", c_i64),
                ("epoch", c_u32), ("flags", c_u32), ("width", c_u32), ("height", c_u32)]


class Picture(ctypes.Structure):
    """media_picture_t: the picture media_engine_step shows (on_present)."""
    _fields_ = [("pts_100ns", c_i64), ("duration_100ns", c_i64), ("epoch", c_u32), ("sequence", c_u32), ("width", c_u32), ("height", c_u32),
                ("luma", c_ptr), ("cb", c_ptr), ("cr", c_ptr), ("luma_stride", c_u32), ("chroma_stride", c_u32), ("flags", c_u32),
                ("view_zoom", c_f32), ("view_x", c_f32), ("view_y", c_f32), ("subtitle", ctypes.c_char_p), ("status", ctypes.c_char_p),
                ("view_roll", c_f32), ("quarter_turns", c_u32), ("mirrored", c_i32), ("view_level", c_f32), ("level_zoom", c_f32)]


DecodeVideoCallback = ctypes.CFUNCTYPE(ctypes.c_int, c_ptr, ctypes.POINTER(VideoUnit))
PresentCallback = ctypes.CFUNCTYPE(None, c_ptr, ctypes.POINTER(Picture))


class Config(ctypes.Structure):
    """media_engine_config_t (176 bytes)."""
    _fields_ = [("struct_size", c_u32), ("modules", c_u32), ("wake_policy", c_i32), ("wake_word", ctypes.c_char * 32), ("voice_input", c_i32),
                ("visible", c_i32), ("maximum_seconds", c_u32), ("on_frame", c_ptr), ("on_frame_context", c_ptr), ("subtitle_path", ctypes.c_char_p),
                ("speak", c_ptr), ("speak_context", c_ptr), ("speak_voices", c_u32), ("playback", c_i32), ("audio_rate", c_u32),
                ("audio_channels", c_u32), ("on_present", c_ptr), ("on_present_context", c_ptr), ("decode_video", c_ptr), ("decode_audio", c_ptr),
                ("decoder_context", c_ptr), ("parent_window", c_ptr), ("output_width", c_u32), ("output_height", c_u32), ("output_flags", c_u32),
                ("output_format", c_u32)]


class Stats(ctypes.Structure):
    """media_engine_stats_t (64 bytes)."""
    _fields_ = [("arena_capacity_bytes", c_u64), ("arena_used_bytes", c_u64), ("module_arena_bytes", c_u64), ("modules", c_u32), ("threads", c_u32),
                ("presented", c_u32), ("dropped", c_u32), ("signatures", c_u32), ("signatures_lost", c_u32), ("commands", c_u32), ("refused", c_u32),
                ("wake_ignored", c_u32), ("wake_templates", c_u32)]


class PlaybackState(ctypes.Structure):
    """media_playback_state_t (88 bytes)."""
    _fields_ = [("position_100ns", c_i64), ("duration_100ns", c_i64), ("epoch", c_u32), ("paused", c_i32), ("muted", c_i32), ("tracking", c_i32),
                ("volume", c_f32), ("speed", c_f32), ("view_zoom", c_f32), ("view_x", c_f32), ("view_y", c_f32), ("audio_track", c_u32),
                ("audio_tracks", c_u32), ("audio_language", ctypes.c_char * 4), ("presented", c_u32), ("dropped", c_u32), ("underruns", c_u32),
                ("saccade", c_i32), ("orientation", c_u32), ("level_degrees", c_f32)]


class DetectedObject(ctypes.Structure):
    """media_detected_object_t (64 bytes): one of the four objects perception keeps track of."""
    _fields_ = [("track_id", c_u32), ("flags", c_u32), ("x", c_f32), ("y", c_f32), ("width", c_f32), ("height", c_f32), ("velocity_x", c_f32),
                ("velocity_y", c_f32), ("ttc_seconds", c_f32), ("divergence", c_f32), ("saliency", c_f32), ("energy", c_f32), ("support", c_u32),
                ("tiles", c_u32), ("age", c_u32), ("reserved", c_u32)]


class FrameTelemetry(ctypes.Structure):
    """media_frame_telemetry_t (472 bytes, API 10's whole struct)."""
    _fields_ = [("struct_size", c_u32), ("flags", c_u32), ("pts_100ns", c_i64), ("sequence", c_u32), ("motion_x", c_f32), ("motion_y", c_f32),
                ("flow_coverage", c_f32), ("divergence", c_f32), ("ttc_seconds", c_f32), ("hazard_x", c_f32), ("hazard_y", c_f32),
                ("salient_fraction", c_f32), ("sound_azimuth", c_f32), ("sound_level_db", c_f32), ("sound_pts_100ns", c_i64), ("sound_events", c_u32),
                ("saccades", c_u32), ("view_zoom", c_f32), ("view_x", c_f32), ("view_y", c_f32), ("view_roll", c_f32), ("horizon_y", c_f32),
                ("focus_x", c_f32), ("estimated_horizon_deg", c_f32), ("active_left", c_u32), ("active_top", c_u32), ("active_right", c_u32),
                ("active_bottom", c_u32), ("level_degrees", c_f32), ("reserved", c_u32 * 2), ("object_count", c_u32), ("target_track_id", c_u32),
                ("objects", DetectedObject * 4), ("vibration_hz", c_f32), ("vibrometry_confidence", c_f32), ("vibration_amplitude", c_f32),
                ("vibrometry_channel", c_u32), ("vital_bpm", c_f32), ("vital_confidence", c_f32), ("vital_channel", c_u32), ("vibrometry_seconds", c_f32),
                ("vibrometry_left", c_f32), ("vibrometry_top", c_f32), ("vibrometry_right", c_f32), ("vibrometry_bottom", c_f32),
                ("vibrometry_flags", c_u32), ("ego_dx", c_f32), ("ego_dy", c_f32), ("ego_turn", c_f32), ("ego_tiles", c_u32), ("ego_inliers", c_u32)]


class TensorRequest(ctypes.Structure):
    """media_tensor_request_t (48 bytes)."""
    _fields_ = [("struct_size", c_u32), ("width", c_u32), ("height", c_u32), ("channels", c_u32), ("format", c_u32), ("normalize", c_u32),
                ("region", c_u32), ("flags", c_u32), ("left", c_f32), ("top", c_f32), ("right", c_f32), ("bottom", c_f32)]


class TensorInfo(ctypes.Structure):
    """media_tensor_info_t (48 bytes)."""
    _fields_ = [("struct_size", c_u32), ("region", c_u32), ("left", c_f32), ("top", c_f32), ("right", c_f32), ("bottom", c_f32), ("pts_100ns", c_i64),
                ("bytes", c_u64), ("target_track_id", c_u32), ("flags", c_u32)]


class EventSpike(ctypes.Structure):
    """media_event_spike_t (16 bytes): one event of the event camera."""
    _fields_ = [("x", c_u16), ("y", c_u16), ("polarity", c_i8), ("contrast", c_u8), ("reserved", c_u16), ("timestamp_us", c_i64)]


class EventCameraConfig(ctypes.Structure):
    """media_event_camera_config_t (32 bytes)."""
    _fields_ = [("struct_size", c_u32), ("contrast_threshold", c_f32), ("max_events_per_pixel", c_u32), ("flags", c_u32), ("reserved", c_u32 * 4)]


class EventCameraState(ctypes.Structure):
    """media_event_camera_state_t (48 bytes)."""
    _fields_ = [("struct_size", c_u32), ("on", c_i32), ("width", c_u32), ("height", c_u32), ("scale", c_u32), ("pictures", c_u32), ("events", c_u64),
                ("dropped", c_u64), ("rebases", c_u32), ("last_events", c_u32)]


class BitstreamInspectorStorage(ctypes.Structure):
    """media_bitstream_inspector_t: 10 KiB of the caller's storage."""
    _fields_ = [("opaque", c_u64 * 1280)]


class BitstreamInfo(ctypes.Structure):
    """media_bitstream_info_t (60 bytes)."""
    _fields_ = [("struct_size", c_u32), ("index", c_u32), ("bytes", c_u32), ("slices", c_u32), ("picture_type", c_u32), ("flags", c_u32), ("qp", c_i32),
                ("complexity", c_f32), ("activity", c_f32), ("burst", c_f32), ("scene_cut_probability", c_f32), ("width", c_u32), ("height", c_u32),
                ("revised_index", c_u32), ("revised_scene_cut_probability", c_f32)]


class OutputTextures(ctypes.Structure):
    """media_output_textures_t (API 14: 80 bytes)."""
    _fields_ = [("struct_size", c_u32), ("count", c_u32), ("width", c_u32), ("height", c_u32), ("format", c_u32), ("generation", c_u32),
                ("textures", c_ptr * 3), ("fence", c_ptr), ("host_fence", c_ptr), ("adapter_luid_low", c_u32), ("adapter_luid_high", c_i32),
                ("color_space", c_u32), ("sdr_white_nits", c_f32)]


class OutputFrame(ctypes.Structure):
    """media_output_frame_t (API 14: 40 bytes)."""
    _fields_ = [("struct_size", c_u32), ("index", c_u32), ("fence_value", c_u64), ("pts_100ns", c_i64), ("generation", c_u32), ("sequence", c_u32),
                ("transfer", c_u32), ("reserved", c_u32)]


# API 15: media_probe, structured controls.
PROBE_MAX_TRACKS = 16
CONTAINERS = {0: "unknown", 1: "mp4", 2: "matroska", 3: "avi", 4: "mpeg-ts", 5: "mpeg-ps", 6: "flv", 7: "asf", 8: "mp3", 9: "flac", 10: "ogg", 11: "wav"}
TRACK_KINDS = {0: "other", 1: "video", 2: "audio", 3: "subtitle"}
CONTROLS = {"seek": 1, "pause": 2, "volume": 3, "mute": 4, "speed": 5, "step_frame": 6, "seek_relative": 7, "audio_track": 8, "subtitle_track": 9}


class ProbeTrack(ctypes.Structure):
    """media_probe_track_t (240 bytes)."""
    _fields_ = [("id", c_u32), ("kind", c_u32), ("codec", ctypes.c_char * 8), ("description", ctypes.c_char * 64), ("supported", c_u32),
                ("width", c_u32), ("height", c_u32), ("channels", c_u32), ("sample_rate", c_u32), ("duration_100ns", c_i64),
                ("language", ctypes.c_char * 4), ("reason", ctypes.c_char * 124)]


class Probe(ctypes.Structure):
    """media_probe_t."""
    _fields_ = [("struct_size", c_u32), ("container", c_u32), ("playable", c_u32), ("track_count", c_u32), ("duration_100ns", c_i64),
                ("reason", ctypes.c_char * 160), ("tracks", ProbeTrack * PROBE_MAX_TRACKS)]


class TrackInfo(ctypes.Structure):
    """media_track_info_t (API 16, 88 bytes)."""
    _fields_ = [("struct_size", c_u32), ("id", c_u32), ("kind", c_u32), ("codec", ctypes.c_char * 8), ("language", ctypes.c_char * 4),
                ("title", ctypes.c_char * 32), ("channels", c_u32), ("sample_rate", c_u32), ("width", c_u32), ("height", c_u32),
                ("selectable", c_u32), ("active", c_u32), ("is_default", c_u32), ("reserved", c_u32)]


class SnapshotBuffer(ctypes.Structure):
    """media_snapshot_buffer_t (API 17, 56 bytes)."""
    _fields_ = [("struct_size", c_u32), ("format", c_u32), ("pixels", c_ptr), ("capacity_bytes", c_u64), ("stride", c_u32), ("width", c_u32),
                ("height", c_u32), ("reserved", c_u32), ("pts_100ns", c_i64), ("frame_hash", c_u64)]


PIXELS = {"bgra8": 1, "rgba8": 2}


class OpenOptions(ctypes.Structure):
    """media_open_options_t (16 bytes)."""
    _fields_ = [("struct_size", c_u32), ("reserved", c_u32), ("http_headers", ctypes.c_char_p)]


SIZES = {SnapshotBuffer: 56, Config: 176, ProbeTrack: 240, Probe: 4024, OpenOptions: 16, TrackInfo: 88, Stats: 64, PlaybackState: 88, DetectedObject: 64, FrameTelemetry: 472, TensorRequest: 48, TensorInfo: 48, EventSpike: 16,
         EventCameraConfig: 32, EventCameraState: 48, BitstreamInspectorStorage: 10240, BitstreamInfo: 60, OutputTextures: 80, OutputFrame: 40,
         VideoUnit: 72}
if ctypes.sizeof(ctypes.c_void_p) == 8:
    for _struct, _size in SIZES.items():
        assert ctypes.sizeof(_struct) == _size, f"{_struct.__name__}: {ctypes.sizeof(_struct)} bytes, the SDK's are {_size}"


def _declare(lib):
    P = ctypes.POINTER
    signatures = {
        "media_api_version": (c_u32, []),
        "media_engine_config_init": (None, [P(Config)]),
        "media_query_arena_budget": (c_u64, [P(Config)]),
        "media_engine_storage_bytes": (ctypes.c_size_t, []),
        "media_engine_storage_alignment": (ctypes.c_size_t, []),
        "media_engine_create": (c_i32, [P(Config), c_ptr, ctypes.c_size_t, P(c_ptr)]),
        "media_engine_open": (c_i32, [c_ptr, ctypes.c_char_p]),
        "media_engine_run": (c_i32, [c_ptr]),
        "media_engine_stop": (None, [c_ptr]),
        "media_engine_close": (c_i32, [c_ptr]),
        "media_engine_destroy": (None, [c_ptr]),
        "media_engine_command": (c_i32, [c_ptr, ctypes.c_char_p]),
        "media_engine_post_command": (c_i32, [c_ptr, ctypes.c_char_p, c_u32]),
        "media_engine_step": (c_i32, [c_ptr, c_i64]),
        "media_engine_pump_audio": (c_i32, [c_ptr, P(c_f32), c_u32, c_i64]),
        "media_engine_state": (c_i32, [c_ptr, P(PlaybackState)]),
        "media_engine_stats": (c_i32, [c_ptr, P(Stats)]),
        "media_engine_last_error": (c_u32, [c_ptr, P(c_i32)]),
        "media_engine_last_error_text": (c_i32, [c_ptr, ctypes.c_char_p, ctypes.c_size_t]),
        "media_probe": (c_i32, [ctypes.c_char_p, P(Probe)]),
        "media_engine_open_ex": (c_i32, [c_ptr, ctypes.c_char_p, P(OpenOptions)]),
        "media_engine_cancel_open": (None, [c_ptr]),
        "media_engine_post_control": (c_i32, [c_ptr, c_u32, ctypes.c_double, c_u32]),
        "media_engine_track_count": (c_i32, [c_ptr, P(c_u32)]),
        "media_engine_track_info": (c_i32, [c_ptr, c_u32, P(TrackInfo)]),
        "media_query_frame_telemetry": (c_i32, [c_ptr, P(FrameTelemetry)]),
        "media_engine_set_stabilization": (c_i32, [c_ptr, ctypes.c_int]),
        "media_engine_track_object": (c_i32, [c_ptr, c_u32]),
        "media_tensor_request_init": (None, [P(TensorRequest)]),
        "media_tensor_bytes": (ctypes.c_size_t, [P(TensorRequest)]),
        "media_export_foveated_tensor": (c_i32, [c_ptr, P(TensorRequest), c_ptr, ctypes.c_size_t, P(TensorInfo)]),
        "media_event_camera_config_init": (None, [P(EventCameraConfig)]),
        "media_engine_set_event_camera": (c_i32, [c_ptr, P(EventCameraConfig)]),
        "media_poll_event_spikes": (c_i32, [c_ptr, P(EventSpike), c_u32, P(c_u32)]),
        "media_query_event_camera": (c_i32, [c_ptr, P(EventCameraState)]),
        "media_bitstream_inspector_init": (c_i32, [P(BitstreamInspectorStorage), c_u32, P(c_u8), c_u32, c_u32]),
        "media_inspect_bitstream_packet": (c_i32, [P(BitstreamInspectorStorage), P(c_u8), c_u32, P(BitstreamInfo)]),
        "media_engine_query_output_textures": (c_i32, [c_ptr, P(OutputTextures)]),
        "media_engine_acquire_output_frame": (c_i32, [c_ptr, P(OutputFrame)]),
        "media_engine_release_output_frame": (c_i32, [c_ptr, c_u32, c_u64]),
        "media_engine_resize_output": (c_i32, [c_ptr, c_u32, c_u32]),
        "media_query_video_decoder": (c_u32, [c_ptr]),
        "media_engine_attach_subtitles": (c_i32, [c_ptr, ctypes.c_char_p]),
        "media_engine_snapshot_to_memory": (c_i32, [c_ptr, P(SnapshotBuffer)]),
    }
    for name, (restype, argtypes) in signatures.items():
        function = getattr(lib, name)
        function.restype, function.argtypes = restype, argtypes
    return lib


def load(path=None):
    """avion.dll from `path`, the AVION_DLL environment variable, or this package's folder."""
    candidates = [path] if path else []
    if os.environ.get("AVION_DLL"):
        candidates.append(os.environ["AVION_DLL"])
    candidates.append(str(pathlib.Path(__file__).resolve().parent / ("avion.dll" if os.name == "nt" else "libavion.so")))
    for candidate in candidates:
        if candidate and os.path.exists(candidate):
            lib = _declare(ctypes.CDLL(candidate))
            if lib.media_api_version() < API_VERSION:
                raise RuntimeError(f"{candidate} is API {lib.media_api_version()}; this binding needs API {API_VERSION}")
            return lib
    raise FileNotFoundError("avion.dll not found (tried: " + ", ".join(c for c in candidates if c) + ")")

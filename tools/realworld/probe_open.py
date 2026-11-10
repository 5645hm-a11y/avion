"""What avion.dll says about media files: media_probe (API 15) and, unless --probe-only, a real media_engine_open as an embedding
app does it, with media_engine_last_error_text when it fails.

    python -I tools/realworld/probe_open.py <avion.dll> [--probe-only] <file> [<file> ...]

The window is created off the screens (visible 0) and closed straight after the open: nothing plays. Exit 1 when any file did
not open (or, with --probe-only, was not playable).
"""
import ctypes, sys, threading

MAX_TRACKS = 16


class Config(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32), ("modules", ctypes.c_uint32), ("_a", ctypes.c_byte * 40), ("visible", ctypes.c_int32),
                ("maximum_seconds", ctypes.c_uint32), ("_b", ctypes.c_byte * 96), ("parent_window", ctypes.c_void_p), ("_c", ctypes.c_byte * 16)]


class ProbeTrack(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint32), ("kind", ctypes.c_uint32), ("codec", ctypes.c_char * 8), ("description", ctypes.c_char * 64),
                ("supported", ctypes.c_uint32), ("width", ctypes.c_uint32), ("height", ctypes.c_uint32), ("channels", ctypes.c_uint32),
                ("sample_rate", ctypes.c_uint32), ("duration_100ns", ctypes.c_int64), ("language", ctypes.c_char * 4), ("reason", ctypes.c_char * 124)]


class Probe(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32), ("container", ctypes.c_uint32), ("playable", ctypes.c_uint32), ("track_count", ctypes.c_uint32),
                ("duration_100ns", ctypes.c_int64), ("reason", ctypes.c_char * 160), ("tracks", ProbeTrack * MAX_TRACKS)]


KINDS = {0: "other", 1: "video", 2: "audio", 3: "subtitle"}
CONTAINERS = {0: "unknown", 1: "MP4/MOV", 2: "Matroska/WebM", 3: "AVI", 4: "MPEG-TS", 5: "MPEG-PS", 6: "FLV", 7: "ASF", 8: "MP3", 9: "FLAC", 10: "Ogg", 11: "WAV"}


def main():
    args = sys.argv[1:]
    probe_only = "--probe-only" in args
    args = [a for a in args if a != "--probe-only"]
    avion = ctypes.CDLL(args[0])
    has_probe = hasattr(avion, "media_probe")
    avion.media_engine_storage_bytes.restype = avion.media_engine_storage_alignment.restype = ctypes.c_size_t
    avion.media_engine_create.argtypes = [ctypes.POINTER(Config), ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p)]
    avion.media_engine_open.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    avion.media_engine_close.argtypes = avion.media_engine_destroy.argtypes = [ctypes.c_void_p]
    avion.media_engine_last_error.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int32)]
    avion.media_engine_last_error.restype = ctypes.c_uint32
    if has_probe:
        avion.media_probe.argtypes = [ctypes.c_char_p, ctypes.POINTER(Probe)]
        avion.media_engine_last_error_text.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
        assert ctypes.sizeof(Probe) == 24 + 160 + MAX_TRACKS * ctypes.sizeof(ProbeTrack), ctypes.sizeof(Probe)
    size, align = avion.media_engine_storage_bytes(), avion.media_engine_storage_alignment()
    storage = ctypes.create_string_buffer(size + align)
    base = (ctypes.addressof(storage) + align - 1) // align * align
    failures = 0
    for path in args[1:]:
        print(path)
        if has_probe:
            p = Probe(); p.struct_size = ctypes.sizeof(Probe)
            st = avion.media_probe(path.encode("utf-8"), ctypes.byref(p))
            print(f"  probe: status {st}, {CONTAINERS.get(p.container, p.container)}, playable {p.playable}, {p.duration_100ns / 1e7:.2f} s"
                  + (f", {p.reason.decode('utf-8', 'replace')}" if p.reason else ""))
            for t in p.tracks[:p.track_count]:
                line = f"    #{t.id} {KINDS.get(t.kind, t.kind):8} {t.codec.decode():5} {t.description.decode('utf-8', 'replace')}"
                line += "" if t.supported else f"  -- {t.reason.decode('utf-8', 'replace')}"
                print(line)
            if probe_only:
                failures += 0 if p.playable else 1
                continue
        result = {}

        def run():
            c = Config(); avion.media_engine_config_init(ctypes.byref(c))
            c.modules = 63 & ~(1 | 2); c.visible = 0
            e = ctypes.c_void_p()
            st = avion.media_engine_create(ctypes.byref(c), base, size, ctypes.byref(e))
            if st != 0:
                result["text"] = f"create failed {st}"; return
            st = avion.media_engine_open(e, path.encode("utf-8"))
            code = ctypes.c_int32(0)
            stage = avion.media_engine_last_error(e, ctypes.byref(code))
            text = ""
            if st != 0 and has_probe:
                buf = ctypes.create_string_buffer(512); avion.media_engine_last_error_text(e, buf, 512); text = buf.value.decode("utf-8", "replace")
            result["text"] = "OK" if st == 0 else f"open status {st}, stage {stage}, code 0x{code.value & 0xffffffff:08x}" + (f": {text}" if text else "")
            result["ok"] = st == 0
            if st == 0: avion.media_engine_close(e)
            avion.media_engine_destroy(e)

        t = threading.Thread(target=run); t.start(); t.join(60)
        failures += 0 if result.get("ok") else 1
        print(f"  open: {result.get('text', 'timed out')}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()

"""A video player in Python with AVION inside (API 12 embedding): tkinter draws the interface, avion.dll plays in a frame of it.
  python avion_player.py video.mp4 [--mute] [--smoke]
--smoke: plays the file muted off the screens, pauses and resumes through the queue, prints every event, exits 0 when it reached
the end (a check that the binding and the DLL agree). avion.dll is looked for next to this file, in ../../bin and ../../build/Release."""
import ctypes, os, pathlib, sys, threading, time, tkinter as tk

HERE = pathlib.Path(__file__).resolve().parent
for folder in (HERE, HERE.parents[1] / "bin", HERE.parents[1] / "build" / "Release"):
    if (folder / "avion.dll").exists():
        avion = ctypes.CDLL(str(folder / "avion.dll"))
        break
else:
    sys.exit("avion.dll not found")

# The C structs as include/media/c_api.h lays them out on x64 (asserted by tests/embed_tests.c).
class Config(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32), ("modules", ctypes.c_uint32), ("_a", ctypes.c_byte * 40), ("visible", ctypes.c_int32),
                ("maximum_seconds", ctypes.c_uint32), ("_b", ctypes.c_byte * 96), ("parent_window", ctypes.c_void_p), ("_c", ctypes.c_byte * 16)]
class Event(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32), ("kind", ctypes.c_uint32), ("time", ctypes.c_int64), ("position", ctypes.c_int64),
                ("duration", ctypes.c_int64), ("latency", ctypes.c_int64), ("command_id", ctypes.c_uint32), ("status", ctypes.c_int32),
                ("paused", ctypes.c_int32), ("muted", ctypes.c_int32), ("volume", ctypes.c_float), ("speed", ctypes.c_float),
                ("width", ctypes.c_uint32), ("height", ctypes.c_uint32), ("reason", ctypes.c_uint32), ("lost", ctypes.c_uint32)]
class State(ctypes.Structure):
    _fields_ = [("position", ctypes.c_int64), ("duration", ctypes.c_int64), ("epoch", ctypes.c_uint32), ("paused", ctypes.c_int32),
                ("muted", ctypes.c_int32), ("tracking", ctypes.c_int32), ("volume", ctypes.c_float), ("speed", ctypes.c_float), ("_rest", ctypes.c_byte * 48)]
assert ctypes.sizeof(Config) == 176 and ctypes.sizeof(Event) == 80 and ctypes.sizeof(State) == 88
KINDS = {1: "opened", 2: "state", 3: "position", 4: "seek landed", 5: "command", 6: "fullscreen request", 7: "ended"}
avion.media_engine_storage_bytes.restype = avion.media_engine_storage_alignment.restype = ctypes.c_size_t
avion.media_engine_create.argtypes = [ctypes.POINTER(Config), ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p)]
for name in ("open", "run", "stop", "close", "destroy", "post_command", "poll_event", "state"):
    getattr(avion, "media_engine_" + name).argtypes = [ctypes.c_void_p] + {"open": [ctypes.c_char_p], "post_command": [ctypes.c_char_p, ctypes.c_uint32],
                                                                         "poll_event": [ctypes.POINTER(Event)], "state": [ctypes.POINTER(State)]}.get(name, [])
assert avion.media_api_version() >= 12, "avion.dll is older than API 12"


class Engine:
    """Create, open, run and close on a thread of its own (the SDK's rule); commands and events from the UI thread."""
    def __init__(self):
        self.storage = ctypes.create_string_buffer(avion.media_engine_storage_bytes() + avion.media_engine_storage_alignment())
        align = avion.media_engine_storage_alignment()
        self.base = (ctypes.addressof(self.storage) + align - 1) // align * align
        self.handle, self.ids, self.released = None, 0, threading.Event()

    def play(self, path, parent):
        opened = threading.Event()
        def run():
            c = Config(); avion.media_engine_config_init(ctypes.byref(c))
            c.modules = 63 & ~(1 | 2); c.parent_window = parent  # No voice, no dock: the interface is tkinter's.
            e = ctypes.c_void_p()
            if avion.media_engine_create(ctypes.byref(c), self.base, avion.media_engine_storage_bytes(), ctypes.byref(e)) == 0:
                if avion.media_engine_open(e, os.fsencode(path) if os.name != "nt" else str(path).encode("utf-8")) == 0:
                    self.handle = e; opened.set()
                    avion.media_engine_run(e)
                    self.released.wait(); avion.media_engine_close(e)
                avion.media_engine_destroy(e)
            opened.set()
        threading.Thread(target=run, daemon=True, name="AVION engine").start()
        opened.wait(10)
        return self.handle is not None

    def command(self, text):
        if not self.handle: return 0
        self.ids += 1
        return self.ids if avion.media_engine_post_command(self.handle, text.encode("utf-8"), self.ids) == 0 else 0

    def poll(self):
        while self.handle:
            ev = Event(struct_size=ctypes.sizeof(Event))
            if avion.media_engine_poll_event(self.handle, ctypes.byref(ev)) != 0: return
            yield ev

    def state(self):
        s = State()
        return s if self.handle and avion.media_engine_state(self.handle, ctypes.byref(s)) == 0 else None

    def stop(self):
        if self.handle: avion.media_engine_stop(self.handle)
        self.handle = None; self.released.set()


def clock(t):
    s = max(0, t) // 10_000_000
    return f"{s // 3600}:{s // 60 % 60:02}:{s % 60:02}" if s >= 3600 else f"{s // 60}:{s % 60:02}"


def main():
    path = next((a for a in sys.argv[1:] if not a.startswith("--")), None)
    smoke = "--smoke" in sys.argv
    if not path: sys.exit(__doc__)
    root = tk.Tk(); root.title("AVION in Python"); root.configure(bg="#121211"); root.geometry("+-4000+-4000" if smoke else "960x600")
    video = tk.Frame(root, bg="black", width=960, height=540); video.pack(fill="both", expand=True)
    bar = tk.Frame(root, bg="#1B1A18"); bar.pack(fill="x")
    style = dict(bg="#1B1A18", fg="#F2EFE9", activebackground="#D97757", relief="flat", font=("Segoe UI Variable Text", 11), padx=12, pady=6)
    engine, paused, ended = Engine(), [False], [None]
    toggle = tk.Button(bar, text="Pause", **style, command=lambda: engine.command("play" if paused[0] else "pause")); toggle.pack(side="left")
    tk.Button(bar, text="−10 s", **style, command=lambda: engine.command("back 10 seconds")).pack(side="left")
    tk.Button(bar, text="+10 s", **style, command=lambda: engine.command("forward 10 seconds")).pack(side="left")
    time_label = tk.Label(bar, text="0:00 / 0:00", **{k: v for k, v in style.items() if k in ("bg", "fg", "font")}); time_label.pack(side="left", padx=14)
    root.update()
    if not engine.play(path, video.winfo_id()): sys.exit("could not open " + path)
    if smoke or "--mute" in sys.argv: engine.command("mute")
    script = [(1.0, "pause"), (1.6, "play")] if smoke else []
    began = time.monotonic()

    def tick():
        for ev in engine.poll():
            if smoke: print(f"{KINDS.get(ev.kind, ev.kind):18} position {ev.position / 1e7:6.2f} s  paused {ev.paused}  status {ev.status}  reason {ev.reason}")
            if ev.kind == 2: paused[0] = bool(ev.paused); toggle.config(text="Play" if paused[0] else "Pause")
            if ev.kind == 7: ended[0] = ev.reason; engine.stop(); root.after(200, root.destroy); return
        if (s := engine.state()): time_label.config(text=f"{clock(s.position)} / {clock(s.duration)}")
        while script and time.monotonic() - began >= script[0][0]: engine.command(script.pop(0)[1])
        root.after(16, tick)
    root.after(16, tick)
    root.protocol("WM_DELETE_WINDOW", lambda: (engine.stop(), root.destroy()))
    root.mainloop()
    if smoke: sys.exit(0 if ended[0] == 0 else 1)


if __name__ == "__main__":
    main()

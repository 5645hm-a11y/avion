// AVION's C99 SDK (include/media/c_api.h, API 12) for .NET: P/Invoke over avion.dll, and AvionEngine, which plays on a thread of
// its own inside a window of yours. The structs are laid out explicitly as the header lays them out on x64 (asserted by
// tests/embed_tests.c), so nothing is marshalled: each call passes the structs' memory as it is.
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Avion;

public static class Native
{
    public const uint ApiVersion = 12;
    public const uint ModuleVoice = 1, ModuleHud = 2, ModuleSubtitles = 4, ModuleNeural = 8, ModuleChapters = 16, ModuleTelemetry = 32, ModuleAll = 63;
    public const int Ok = 0, ErrorEmpty = 8, ErrorRefused = 7;

    [StructLayout(LayoutKind.Explicit, Size = 176)]
    public struct Config
    {
        [FieldOffset(0)] public uint StructSize;
        [FieldOffset(4)] public uint Modules;
        [FieldOffset(48)] public int Visible;
        [FieldOffset(52)] public uint MaximumSeconds;
        [FieldOffset(100)] public int Playback;         // 0: the engine's window (here: inside yours), 1: host mode.
        [FieldOffset(152)] public IntPtr ParentWindow;  // API 12: your window's HWND.
    }

    public enum EventKind : uint { Opened = 1, State = 2, Position = 3, SeekLanded = 4, Command = 5, FullscreenRequest = 6, Ended = 7 }
    public enum EndReason : uint { Media = 0, Stopped = 1, Closed = 2, Error = 3, Limit = 4 }

    [StructLayout(LayoutKind.Explicit, Size = 80)]
    public struct Event
    {
        [FieldOffset(0)] public uint StructSize;
        [FieldOffset(4)] public EventKind Kind;
        [FieldOffset(8)] public long Time;
        [FieldOffset(16)] public long Position;     // 100 ns units.
        [FieldOffset(24)] public long Duration;
        [FieldOffset(32)] public long Latency;
        [FieldOffset(40)] public uint CommandId;
        [FieldOffset(44)] public int Status;
        [FieldOffset(48)] public int Paused;
        [FieldOffset(52)] public int Muted;
        [FieldOffset(56)] public float Volume;      // 0..1.
        [FieldOffset(60)] public float Speed;
        [FieldOffset(64)] public uint Width;
        [FieldOffset(68)] public uint Height;
        [FieldOffset(72)] public EndReason Reason;
        [FieldOffset(76)] public uint Lost;
    }

    [StructLayout(LayoutKind.Explicit, Size = 88)]
    public struct State
    {
        [FieldOffset(0)] public long Position;
        [FieldOffset(8)] public long Duration;
        [FieldOffset(20)] public int Paused;
        [FieldOffset(24)] public int Muted;
        [FieldOffset(32)] public float Volume;
        [FieldOffset(36)] public float Speed;
        [FieldOffset(64)] public uint Presented;
        [FieldOffset(68)] public uint Dropped;
    }

    const string Dll = "avion.dll";
    [DllImport(Dll)] public static extern uint media_api_version();
    [DllImport(Dll)] public static extern void media_engine_config_init(ref Config config);
    [DllImport(Dll)] public static extern nuint media_engine_storage_bytes();
    [DllImport(Dll)] public static extern nuint media_engine_storage_alignment();
    [DllImport(Dll)] public static extern int media_engine_create(ref Config config, IntPtr storage, nuint bytes, out IntPtr engine);
    [DllImport(Dll)] public static extern int media_engine_open(IntPtr engine, byte[] utf8Path);
    [DllImport(Dll)] public static extern int media_engine_run(IntPtr engine);
    [DllImport(Dll)] public static extern void media_engine_stop(IntPtr engine);
    [DllImport(Dll)] public static extern int media_engine_close(IntPtr engine);
    [DllImport(Dll)] public static extern void media_engine_destroy(IntPtr engine);
    [DllImport(Dll)] public static extern int media_engine_post_command(IntPtr engine, byte[] utf8, uint commandId);
    [DllImport(Dll)] public static extern int media_engine_poll_event(IntPtr engine, ref Event e);
    [DllImport(Dll)] public static extern int media_engine_state(IntPtr engine, ref State state);

    public static byte[] Utf8(string text) => Encoding.UTF8.GetBytes(text + "\0");
}

// One engine at a time, playing inside `parent`: create, open, run and close on the engine's thread (the SDK's rule), commands
// and events from your UI thread. Poll() returns the next event, or null.
public sealed unsafe class AvionEngine : IDisposable
{
    readonly void* storage;
    readonly nuint storageBytes;
    Thread? thread;
    IntPtr engine;                       // Set once open succeeded; cleared before the engine is destroyed.
    volatile bool running;               // Between open and run's return: media_engine_stop may be called.
    readonly ManualResetEventSlim release = new(false);
    uint nextId = 1;

    public AvionEngine()
    {
        if (Native.media_api_version() < Native.ApiVersion) throw new InvalidOperationException("avion.dll is older than API 12");
        storageBytes = Native.media_engine_storage_bytes();
        storage = NativeMemory.AlignedAlloc(storageBytes, Native.media_engine_storage_alignment());
    }

    public bool Playing => Volatile.Read(ref engine) != IntPtr.Zero;

    public void Play(string path, IntPtr parent)
    {
        Stop();
        release.Reset();
        var opened = new ManualResetEventSlim(false);
        thread = new Thread(() =>
        {
            var config = new Native.Config();
            Native.media_engine_config_init(ref config);
            if (config.StructSize != Marshal.SizeOf<Native.Config>()) throw new InvalidOperationException("Config layout differs from avion.dll's");
            config.Modules = Native.ModuleAll & ~(Native.ModuleVoice | Native.ModuleHud); // Your interface, not the engine's dock.
            config.ParentWindow = parent;
            if (Native.media_engine_create(ref config, (IntPtr)storage, storageBytes, out var e) != Native.Ok) { opened.Set(); return; }
            if (Native.media_engine_open(e, Native.Utf8(path)) == Native.Ok)
            {
                running = true;
                Volatile.Write(ref engine, e);
                opened.Set();
                Native.media_engine_run(e);       // Blocks until the end, Stop(), or your window closing.
                running = false;
                release.Wait();                   // The UI thread has stopped polling (a late Stop still finds the engine open).
                Native.media_engine_close(e);
            }
            else opened.Set();
            Native.media_engine_destroy(e);
        }) { IsBackground = true, Name = "AVION engine" };
        thread.Start();
        opened.Wait(10000);
    }

    // A command of the grammar ("pause", "volume 40", "go to 1:23", "forward 10 seconds", "mute", ...); its id comes back in a
    // Command event.
    public uint Command(string text)
    {
        var e = Volatile.Read(ref engine);
        if (e == IntPtr.Zero) return 0;
        var id = nextId++;
        return Native.media_engine_post_command(e, Native.Utf8(text), id) == Native.Ok ? id : 0;
    }

    public Native.Event? Poll()
    {
        var e = Volatile.Read(ref engine);
        if (e == IntPtr.Zero) return null;
        var ev = new Native.Event { StructSize = (uint)Marshal.SizeOf<Native.Event>() };
        return Native.media_engine_poll_event(e, ref ev) == Native.Ok ? ev : null;
    }

    public Native.State? State()
    {
        var e = Volatile.Read(ref engine);
        var s = new Native.State();
        return e != IntPtr.Zero && Native.media_engine_state(e, ref s) == Native.Ok ? s : null;
    }

    // After an Ended event (or to end playback now): lets the engine's thread destroy the engine.
    public void Stop()
    {
        var e = Interlocked.Exchange(ref engine, IntPtr.Zero);
        if (e != IntPtr.Zero && running) Native.media_engine_stop(e);
        release.Set();
        thread?.Join(5000);
        thread = null;
    }

    public void Dispose()
    {
        Stop();
        NativeMemory.AlignedFree(storage);
        release.Dispose();
    }
}

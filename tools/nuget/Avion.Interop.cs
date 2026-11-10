// AVION's C99 SDK (include/media/c_api.h, API 17) for .NET (netstandard2.0: .NET 6/8, .NET Framework 4.6.1+, Unity): P/Invoke over
// avion.dll. The structs are laid out explicitly as the header lays them out on x64 (offsets asserted by src/sdk/CApi.cpp and
// tests/embed_tests.c), so nothing is marshalled field by field: each call passes the struct's memory as it is.
//   AvionHost:   host mode: your thread steps the engine (telemetry, tensors, the GPU decoding into shared textures).
//   AvionWindow: the engine's own thread plays inside a window of yours (embedding, API 12).
// Licence: GNU AGPLv3 (LICENSE) or a commercial licence (LICENSING.md in this package). Source: https://github.com/5645hm-a11y/avion
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Avion
{
    public static class Native
    {
        public const uint ApiVersion = 17;
        public const uint ModuleVoice = 1, ModuleHud = 2, ModuleSubtitles = 4, ModuleNeural = 8, ModuleChapters = 16, ModuleTelemetry = 32, ModuleAll = 63;
        public const int Ok = 0, ErrorInvalidArgument = 1, ErrorState = 3, ErrorUnsupported = 4, ErrorOpen = 5, ErrorPlayback = 6, ErrorRefused = 7, ErrorEmpty = 8;
        public const int PlaybackWindow = 0, PlaybackHost = 1;
        public const uint OutputTexture = 1;
        public const uint OutputFormatRgba8 = 0, OutputFormatPq10 = 1, OutputFormatScrgb16 = 2;
        public const uint DecoderNone = 0, DecoderSoftware = 1, DecoderHost = 2, DecoderHardware = 3;
        public const uint TensorF32 = 1, TensorF16 = 2, TensorFovea = 0, TensorView = 1, TensorFull = 2, TensorRegion = 3;
        public const uint TelemetryLooming = 2, TelemetryObjects = 1024, TelemetryVibration = 2048, TelemetryVitals = 4096, TelemetryVirtualImu = 8192;

        [StructLayout(LayoutKind.Explicit, Size = 176)]
        public struct Config
        {
            [FieldOffset(0)] public uint StructSize;
            [FieldOffset(4)] public uint Modules;
            [FieldOffset(48)] public int Visible;
            [FieldOffset(52)] public uint MaximumSeconds;
            [FieldOffset(100)] public int Playback;          // PlaybackWindow or PlaybackHost.
            [FieldOffset(104)] public uint AudioRate;
            [FieldOffset(108)] public uint AudioChannels;
            [FieldOffset(152)] public IntPtr ParentWindow;   // API 12: your window's HWND.
            [FieldOffset(160)] public uint OutputWidth;      // API 13: texture output.
            [FieldOffset(164)] public uint OutputHeight;
            [FieldOffset(168)] public uint OutputFlags;
            [FieldOffset(172)] public uint OutputFormat;     // API 14: OutputFormat*.
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
            [FieldOffset(56)] public float Volume;
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

        [StructLayout(LayoutKind.Explicit, Size = 64)]
        public struct DetectedObject
        {
            [FieldOffset(0)] public uint TrackId;
            [FieldOffset(4)] public uint Flags;
            [FieldOffset(8)] public float X;
            [FieldOffset(12)] public float Y;
            [FieldOffset(16)] public float Width;
            [FieldOffset(20)] public float Height;
            [FieldOffset(24)] public float VelocityX;
            [FieldOffset(28)] public float VelocityY;
            [FieldOffset(32)] public float TtcSeconds;
            [FieldOffset(40)] public float Saliency;
            [FieldOffset(56)] public uint Age;
        }

        // media_frame_telemetry_t (472 bytes): the fields most hosts read; the rest stays in the struct's memory.
        [StructLayout(LayoutKind.Explicit, Size = 472)]
        public struct FrameTelemetry
        {
            [FieldOffset(0)] public uint StructSize;
            [FieldOffset(4)] public uint Flags;
            [FieldOffset(8)] public long Pts;
            [FieldOffset(16)] public uint Sequence;
            [FieldOffset(36)] public float TtcSeconds;
            [FieldOffset(136)] public uint ObjectCount;
            [FieldOffset(140)] public uint TargetTrackId;
            [FieldOffset(144)] public DetectedObject Object0;
            [FieldOffset(208)] public DetectedObject Object1;
            [FieldOffset(272)] public DetectedObject Object2;
            [FieldOffset(336)] public DetectedObject Object3;
            [FieldOffset(400)] public float VibrationHz;
            [FieldOffset(404)] public float VibrometryConfidence;
            [FieldOffset(416)] public float VitalBpm;          // Remote photoplethysmography (MEDIA_TELEMETRY_VITALS).
            [FieldOffset(420)] public float VitalConfidence;
            [FieldOffset(428)] public float VibrometrySeconds;
            [FieldOffset(452)] public float EgoDx;             // The virtual IMU (API 10).
            [FieldOffset(456)] public float EgoDy;
            [FieldOffset(460)] public float EgoTurn;
        }

        [StructLayout(LayoutKind.Explicit, Size = 48)]
        public struct TensorRequest
        {
            [FieldOffset(0)] public uint StructSize;
            [FieldOffset(4)] public uint Width;
            [FieldOffset(8)] public uint Height;
            [FieldOffset(12)] public uint Channels;
            [FieldOffset(16)] public uint Format;
            [FieldOffset(20)] public uint Normalize;
            [FieldOffset(24)] public uint Region;
            [FieldOffset(28)] public uint Flags;
            [FieldOffset(32)] public float Left;
            [FieldOffset(36)] public float Top;
            [FieldOffset(40)] public float Right;
            [FieldOffset(44)] public float Bottom;
        }

        [StructLayout(LayoutKind.Explicit, Size = 48)]
        public struct TensorInfo
        {
            [FieldOffset(0)] public uint StructSize;
            [FieldOffset(4)] public uint Region;
            [FieldOffset(24)] public long Pts;
            [FieldOffset(32)] public ulong Bytes;
            [FieldOffset(40)] public uint TargetTrackId;
            [FieldOffset(44)] public uint Flags;
        }

        [StructLayout(LayoutKind.Explicit, Size = 80)]
        public struct OutputTextures
        {
            [FieldOffset(0)] public uint StructSize;
            [FieldOffset(4)] public uint Count;
            [FieldOffset(8)] public uint Width;
            [FieldOffset(12)] public uint Height;
            [FieldOffset(16)] public uint Format;            // DXGI_FORMAT: 28 R8G8B8A8_UNORM, 24 R10G10B10A2_UNORM, 10 R16G16B16A16_FLOAT.
            [FieldOffset(20)] public uint Generation;
            [FieldOffset(24)] public IntPtr Texture0;        // NT handles: ID3D12Device::OpenSharedHandle / ID3D11Device1::OpenSharedResource1.
            [FieldOffset(32)] public IntPtr Texture1;
            [FieldOffset(40)] public IntPtr Texture2;
            [FieldOffset(48)] public IntPtr Fence;
            [FieldOffset(56)] public IntPtr HostFence;
            [FieldOffset(64)] public uint AdapterLuidLow;
            [FieldOffset(68)] public int AdapterLuidHigh;
            [FieldOffset(72)] public uint ColorSpace;        // DXGI_COLOR_SPACE_TYPE (API 14).
            [FieldOffset(76)] public float SdrWhiteNits;
        }

        [StructLayout(LayoutKind.Explicit, Size = 40)]
        public struct OutputFrame
        {
            [FieldOffset(0)] public uint StructSize;
            [FieldOffset(4)] public uint Index;
            [FieldOffset(8)] public ulong FenceValue;
            [FieldOffset(16)] public long Pts;
            [FieldOffset(24)] public uint Generation;
            [FieldOffset(28)] public uint Sequence;
            [FieldOffset(32)] public uint Transfer;          // H.273: 1 BT.709, 16 PQ, 18 HLG (API 14).
        }

        const string Dll = "avion.dll";
        [DllImport(Dll)] public static extern uint media_api_version();
        // API 17: subtitles after open (UTF-8 path) and a snapshot into the caller's memory.
        [StructLayout(LayoutKind.Sequential)]
        public struct SnapshotBuffer {
            public uint struct_size, format; public IntPtr pixels; public ulong capacity_bytes; public uint stride, width, height, reserved;
            public long pts_100ns; public ulong frame_hash;
        }
        public const uint PixelsBgra8 = 1, PixelsRgba8 = 2;
        [DllImport(Dll)] public static extern int media_engine_attach_subtitles(IntPtr engine, byte[] utf8_path);
        [DllImport(Dll)] public static extern int media_engine_snapshot_to_memory(IntPtr engine, ref SnapshotBuffer buffer);
        // API 15: structured controls, the failure in words, cancelling an open (media_probe's struct: see docs/sdk-reference.md).
        [DllImport(Dll)] public static extern int media_engine_post_control(IntPtr engine, uint control, double value, uint commandId);
        [DllImport(Dll)] public static extern int media_engine_last_error_text(IntPtr engine, byte[] text, UIntPtr bytes);
        [DllImport(Dll)] public static extern void media_engine_cancel_open(IntPtr engine);
        // API 16: the tracks (windowed engine), as media_track_info_t (88 bytes: read it with Marshal from a byte buffer).
        [DllImport(Dll)] public static extern int media_engine_track_count(IntPtr engine, out uint count);
        [DllImport(Dll)] public static extern int media_engine_track_info(IntPtr engine, uint index, byte[] info);
        [DllImport(Dll)] public static extern void media_engine_config_init(ref Config config);
        [DllImport(Dll)] public static extern UIntPtr media_engine_storage_bytes();
        [DllImport(Dll)] public static extern UIntPtr media_engine_storage_alignment();
        [DllImport(Dll)] public static extern int media_engine_create(ref Config config, IntPtr storage, UIntPtr bytes, out IntPtr engine);
        [DllImport(Dll)] public static extern int media_engine_open(IntPtr engine, byte[] utf8Path);
        [DllImport(Dll)] public static extern int media_engine_run(IntPtr engine);
        [DllImport(Dll)] public static extern void media_engine_stop(IntPtr engine);
        [DllImport(Dll)] public static extern int media_engine_close(IntPtr engine);
        [DllImport(Dll)] public static extern void media_engine_destroy(IntPtr engine);
        [DllImport(Dll)] public static extern int media_engine_command(IntPtr engine, byte[] utf8);
        [DllImport(Dll)] public static extern int media_engine_post_command(IntPtr engine, byte[] utf8, uint commandId);
        [DllImport(Dll)] public static extern int media_engine_poll_event(IntPtr engine, ref Event e);
        [DllImport(Dll)] public static extern int media_engine_state(IntPtr engine, ref State state);
        [DllImport(Dll)] public static extern int media_engine_step(IntPtr engine, long now100ns);
        [DllImport(Dll)] public static extern int media_engine_pump_audio(IntPtr engine, float[] output, uint frames, long now100ns);
        [DllImport(Dll)] public static extern int media_query_frame_telemetry(IntPtr engine, ref FrameTelemetry telemetry);
        [DllImport(Dll)] public static extern void media_tensor_request_init(ref TensorRequest request);
        [DllImport(Dll)] public static extern UIntPtr media_tensor_bytes(ref TensorRequest request);
        [DllImport(Dll)] public static extern int media_export_foveated_tensor(IntPtr engine, ref TensorRequest request, float[] tensor, UIntPtr bytes, ref TensorInfo info);
        [DllImport(Dll)] public static extern int media_engine_query_output_textures(IntPtr engine, ref OutputTextures textures);
        [DllImport(Dll)] public static extern int media_engine_acquire_output_frame(IntPtr engine, ref OutputFrame frame);
        [DllImport(Dll)] public static extern int media_engine_release_output_frame(IntPtr engine, uint index, ulong hostFenceValue);
        [DllImport(Dll)] public static extern int media_engine_resize_output(IntPtr engine, uint width, uint height);
        [DllImport(Dll)] public static extern uint media_query_video_decoder(IntPtr engine);

        public static byte[] Utf8(string text) => Encoding.UTF8.GetBytes(text + "\0");
        public static void Check(string call, int status) { if (status != Ok) throw new AvionException(call, status); }
    }

    public sealed class AvionException : Exception
    {
        public int Status { get; }
        public AvionException(string call, int status) : base(call + " returned " + status) { Status = status; }
    }

    // The engine's storage, aligned as the SDK asks (it allocates nothing itself).
    sealed class Storage : IDisposable
    {
        readonly IntPtr raw;
        public IntPtr Base { get; }
        public UIntPtr Bytes { get; }
        public Storage()
        {
            if (Native.media_api_version() < Native.ApiVersion) throw new InvalidOperationException("avion.dll is older than API " + Native.ApiVersion);
            Bytes = Native.media_engine_storage_bytes();
            var align = (long)(ulong)Native.media_engine_storage_alignment();
            raw = Marshal.AllocHGlobal(new IntPtr((long)(ulong)Bytes + align));
            Base = new IntPtr((raw.ToInt64() + align - 1) / align * align);
        }
        public void Dispose() => Marshal.FreeHGlobal(raw);
    }

    // Host mode: your thread opens, steps and queries the engine (no engine thread). With a texture size, on Windows the engine
    // decodes on the GPU into three shared Direct3D textures (OutputTextures, Acquire, Release); OutputFormat* chooses 8-bit, PQ10
    // or scRGB FP16.
    public sealed class AvionHost : IDisposable
    {
        readonly Storage storage = new Storage();
        IntPtr engine;
        readonly float[] audio = new float[480 * 2];
        long audioDue;
        float[] tensor = new float[0];

        public AvionHost(uint textureWidth = 0, uint textureHeight = 0, uint outputFormat = Native.OutputFormatRgba8)
        {
            var c = new Native.Config();
            Native.media_engine_config_init(ref c);
            c.Modules = Native.ModuleAll & ~(Native.ModuleVoice | Native.ModuleHud);
            c.Playback = Native.PlaybackHost; c.AudioRate = 48000; c.AudioChannels = 2;
            if (textureWidth != 0) { c.OutputFlags = Native.OutputTexture; c.OutputWidth = textureWidth; c.OutputHeight = textureHeight; c.OutputFormat = outputFormat; }
            Native.Check("media_engine_create", Native.media_engine_create(ref c, storage.Base, storage.Bytes, out engine));
        }
        public void Open(string path) { Native.Check("media_engine_open", Native.media_engine_open(engine, Native.Utf8(path))); audioDue = 0; }
        // One pass at `now100ns` (your monotonic clock) and the audio due since the last one; false at the end of the media.
        public bool Step(long now100ns)
        {
            var status = Native.media_engine_step(engine, now100ns);
            if (status == Native.ErrorEmpty) return false;
            Native.Check("media_engine_step", status);
            for (; audioDue <= now100ns; audioDue += 100000) Native.media_engine_pump_audio(engine, audio, 480, audioDue);
            return true;
        }
        public bool Command(string text) => Native.media_engine_command(engine, Native.Utf8(text)) == Native.Ok;
        public string VideoDecoder
        {
            get { switch (Native.media_query_video_decoder(engine)) { case Native.DecoderHardware: return "hardware"; case Native.DecoderHost: return "host"; case Native.DecoderSoftware: return "software"; default: return "none"; } }
        }
        public Native.FrameTelemetry? Telemetry()
        {
            var t = new Native.FrameTelemetry { StructSize = (uint)Marshal.SizeOf<Native.FrameTelemetry>() };
            return Native.media_query_frame_telemetry(engine, ref t) == Native.Ok ? t : (Native.FrameTelemetry?)null;
        }
        // NCHW float32 (1 x 3 x height x width) of the fovea, into a buffer this object reuses.
        public float[] ExportTensor(uint width = 224, uint height = 224, uint region = Native.TensorFovea)
        {
            var r = new Native.TensorRequest(); Native.media_tensor_request_init(ref r);
            r.Width = width; r.Height = height; r.Region = region;
            var bytes = Native.media_tensor_bytes(ref r);
            if (tensor.Length * 4 != (long)(ulong)bytes) tensor = new float[(long)(ulong)bytes / 4];
            var info = new Native.TensorInfo { StructSize = (uint)Marshal.SizeOf<Native.TensorInfo>() };
            Native.Check("media_export_foveated_tensor", Native.media_export_foveated_tensor(engine, ref r, tensor, bytes, ref info));
            return tensor;
        }
        public Native.OutputTextures? OutputTextures()
        {
            var t = new Native.OutputTextures { StructSize = (uint)Marshal.SizeOf<Native.OutputTextures>() };
            return Native.media_engine_query_output_textures(engine, ref t) == Native.Ok ? t : (Native.OutputTextures?)null;
        }
        public Native.OutputFrame? Acquire()
        {
            var f = new Native.OutputFrame { StructSize = (uint)Marshal.SizeOf<Native.OutputFrame>() };
            return Native.media_engine_acquire_output_frame(engine, ref f) == Native.Ok ? f : (Native.OutputFrame?)null;
        }
        public void Release(uint index, ulong hostFenceValue) => Native.Check("media_engine_release_output_frame", Native.media_engine_release_output_frame(engine, index, hostFenceValue));
        public void Dispose()
        {
            if (engine != IntPtr.Zero) { Native.media_engine_destroy(engine); engine = IntPtr.Zero; }
            storage.Dispose();
        }
    }

    // The engine's own thread plays inside `parent` (API 12): create, open, run and close there (the SDK's rule); commands and events
    // from your UI thread. Poll() returns the next event, or null.
    public sealed class AvionWindow : IDisposable
    {
        readonly Storage storage = new Storage();
        Thread thread;
        IntPtr engine;
        volatile bool running;
        readonly ManualResetEventSlim release = new ManualResetEventSlim(false);
        uint nextId = 1;

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
                config.Modules = Native.ModuleAll & ~(Native.ModuleVoice | Native.ModuleHud);
                config.ParentWindow = parent;
                if (Native.media_engine_create(ref config, storage.Base, storage.Bytes, out var e) != Native.Ok) { opened.Set(); return; }
                if (Native.media_engine_open(e, Native.Utf8(path)) == Native.Ok)
                {
                    running = true; Volatile.Write(ref engine, e); opened.Set();
                    Native.media_engine_run(e);
                    running = false; release.Wait(); Native.media_engine_close(e);
                }
                else opened.Set();
                Native.media_engine_destroy(e);
            }) { IsBackground = true, Name = "AVION engine" };
            thread.Start();
            opened.Wait(10000);
        }
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
            return Native.media_engine_poll_event(e, ref ev) == Native.Ok ? ev : (Native.Event?)null;
        }
        public void Stop()
        {
            var e = Interlocked.Exchange(ref engine, IntPtr.Zero);
            if (e != IntPtr.Zero && running) Native.media_engine_stop(e);
            release.Set();
            thread?.Join(5000);
            thread = null;
        }
        public void Dispose() { Stop(); storage.Dispose(); release.Dispose(); }
    }
}

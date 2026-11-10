# Avion.Sdk

The AVION media engine for .NET and native C/C++ on Windows x64 (C99 SDK, API 14).

```
dotnet add package Avion.Sdk --source <folder with Avion.Sdk.0.1.0.nupkg>
```

```csharp
using Avion;

using var host = new AvionHost(textureWidth: 1920, textureHeight: 1080, outputFormat: Native.OutputFormatPq10);
host.Open("clip.mp4");                                   // H.264 / HEVC decoded on the GPU into shared Direct3D textures.
var clock = System.Diagnostics.Stopwatch.StartNew();
while (host.Step(clock.Elapsed.Ticks))                   // 100 ns ticks.
{
    if (host.Acquire() is Native.OutputFrame frame)      // Open OutputTextures() once with ID3D12Device::OpenSharedHandle,
        host.Release(frame.Index, 0);                    // wait for frame.FenceValue on your queue, draw, release.
    if (host.Telemetry() is Native.FrameTelemetry t && (t.Flags & Native.TelemetryVitals) != 0)
        System.Console.WriteLine($"pulse {t.VitalBpm:F0} BPM");
}
```

- `lib/netstandard2.0/Avion.Interop.dll`: `Avion.Native` (P/Invoke, explicit x64 layouts), `AvionHost` (host mode),
  `AvionWindow` (the engine inside your window). `src/Avion.Interop.cs` is the same code as source, for Unity.
- `runtimes/win-x64/native/avion.dll`: the engine (Windows libraries only).
- Native C/C++ (MSBuild): `build/native/Avion.Sdk.targets` adds `include/media/c_api.h`, links `avion.lib` and copies
  `avion.dll` next to your program.

Dual-licensed: the GNU AGPLv3 (`LICENSE`) or a commercial licence from the author for closed-source software, devices and services (`LICENSING.md`). Source: https://github.com/5645hm-a11y/avion

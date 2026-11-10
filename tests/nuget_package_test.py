"""media_nuget_package_test (API 16): the NuGet package as a .NET developer gets it.

    python -I tests/nuget_package_test.py <avion.dll> <avion.lib> <fixtures folder>

Builds Avion.Sdk.0.1.0.nupkg (tools/nuget/build_nuget.py), checks its layout and nuspec, then makes a fresh .NET 8 console project
that references it from a local folder only (its own nuget.config and packages folder: nothing from elsewhere), builds it, and runs
it: the package's avion.dll must land next to the program (runtimes/win-x64/native), Avion.Interop's AvionHost plays
playback_chapters.mp4 with texture output (the GPU decoder, frames acquired and released) and playback_pulse.mp4 on the software
bridge (the pulse read through the telemetry struct). Exit 0 when every check passed."""
import pathlib, shutil, subprocess, sys, tempfile, zipfile

PROGRAM = r'''
using System;
using System.Diagnostics;
using Avion;

static class Program
{
    static int failures;
    static void Check(bool ok, string what) { if (!ok) { failures++; Console.WriteLine("FAIL: " + what); } }
    static int Main(string[] args)
    {
        Check(Native.media_api_version() == 17, "API " + Native.media_api_version());
        using (var host = new AvionHost(320, 180, Native.OutputFormatPq10))
        {
            host.Open(args[0]);
            Check(host.VideoDecoder == "hardware", "decoder " + host.VideoDecoder);
            var clock = Stopwatch.StartNew(); int frames = 0; uint format = 0;
            while (clock.Elapsed.TotalSeconds < 3 && host.Step(clock.Elapsed.Ticks))
            {
                var t = host.OutputTextures(); if (t.HasValue) format = t.Value.Format;
                var f = host.Acquire();
                if (f.HasValue) { frames++; host.Release(f.Value.Index, 0); }
                System.Threading.Thread.Sleep(2);
            }
            Check(frames >= 30 && format == 24, $"{frames} frames, DXGI format {format}");
            Console.WriteLine($"texture output: {frames} frames in 3 s from the {host.VideoDecoder} decoder, DXGI format {format}");
        }
        using (var host = new AvionHost())
        {
            host.Open(args[1]);
            Check(host.VideoDecoder == "software", "pulse decoder " + host.VideoDecoder);
            float bpm = 0; long now = 0;
            for (int i = 0; i < 11 * 48 && host.Step(now); ++i, now += 10000000 / 48)
            {
                var t = host.Telemetry();
                if (t.HasValue && (t.Value.Flags & Native.TelemetryVitals) != 0) bpm = t.Value.VitalBpm;
            }
            var tensor = host.ExportTensor();
            Check(Math.Abs(bpm - 72) <= 2, "pulse " + bpm);
            Check(tensor.Length == 3 * 224 * 224, "tensor " + tensor.Length);
            Console.WriteLine($"telemetry: pulse {bpm:F1} BPM; tensor of {tensor.Length} floats");
        }
        Console.WriteLine(failures == 0 ? "nuget package: all checks passed" : $"nuget package: {failures} FAILED");
        return failures == 0 ? 0 : 1;
    }
}
'''


def main():
    dll, lib, fixtures = (pathlib.Path(a).resolve() for a in sys.argv[1:4])
    root = pathlib.Path(__file__).resolve().parents[1]
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="avion_nuget_test_"))
    failures = 0
    try:
        built = subprocess.run([sys.executable, "-I", str(root / "tools" / "nuget" / "build_nuget.py"), str(dll), str(lib), str(scratch / "feed")],
                               capture_output=True, text=True)
        if built.returncode != 0:
            print("FAIL: build_nuget", built.stdout, built.stderr); return 1
        package = pathlib.Path(built.stdout.strip().splitlines()[-1])
        with zipfile.ZipFile(package) as z:
            names = set(z.namelist())
            nuspec = z.read("Avion.Sdk.nuspec").decode("utf-8")
        need = {"Avion.Sdk.nuspec", "lib/netstandard2.0/Avion.Interop.dll", "runtimes/win-x64/native/avion.dll", "build/native/Avion.Sdk.targets",
                "build/native/include/media/c_api.h", "build/native/lib/x64/avion.lib", "src/Avion.Interop.cs", "LICENSE", "LICENSING.md", "README.md"}
        if not need <= names or "<id>Avion.Sdk</id>" not in nuspec or '<license type="expression">AGPL-3.0-only</license>' not in nuspec:
            print("FAIL: package layout", sorted(names)); failures += 1
        print(f"package {package.name}: {len(names)} entries, {package.stat().st_size} bytes")
        app = scratch / "app"; app.mkdir()
        (app / "nuget.config").write_text(f'''<?xml version="1.0" encoding="utf-8"?>
<configuration><packageSources><clear /><add key="local" value="{scratch / 'feed'}" /></packageSources>
<config><add key="globalPackagesFolder" value="{scratch / 'packages'}" /></config></configuration>''', encoding="utf-8")
        (app / "AvionCheck.csproj").write_text('''<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework><RuntimeIdentifier>win-x64</RuntimeIdentifier>
    <SelfContained>false</SelfContained><Nullable>disable</Nullable><TreatWarningsAsErrors>true</TreatWarningsAsErrors></PropertyGroup>
  <ItemGroup><PackageReference Include="Avion.Sdk" Version="0.1.0" /></ItemGroup>
</Project>''', encoding="utf-8")
        (app / "Program.cs").write_text(PROGRAM, encoding="utf-8")
        build = subprocess.run(["dotnet", "build", str(app / "AvionCheck.csproj"), "-c", "Release", "-nologo", "-v", "q", "-o", str(app / "out")],
                               capture_output=True, text=True, cwd=app)
        if build.returncode != 0:
            print("FAIL: dotnet build", build.stdout[-3000:], build.stderr[-2000:]); return 1
        if not (app / "out" / "avion.dll").exists():
            print("FAIL: avion.dll not copied next to the program"); failures += 1
        run = subprocess.run([str(app / "out" / "AvionCheck.exe"), str(fixtures / "playback_chapters.mp4"), str(fixtures / "playback_pulse.mp4")],
                             capture_output=True, text=True, timeout=120)
        print(run.stdout.strip())
        if run.returncode != 0:
            failures += 1; print(run.stderr.strip())
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print("nuget test: " + (f"{failures} FAILED" if failures else "all checks passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

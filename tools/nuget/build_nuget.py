"""Builds the AVION NuGet package (Avion.Sdk.<version>.nupkg).

    python tools/nuget/build_nuget.py <avion.dll> <avion.lib> <out_dir>

1. Compiles tools/nuget/Avion.Interop.cs into Avion.Interop.dll (netstandard2.0, Release) with the .NET SDK on PATH (`dotnet`),
   in a scratch folder.
2. Writes the package (a zip, OPC layout) with the standard library:
     Avion.Sdk.nuspec                         id, version, author, the licence file, the description
     lib/netstandard2.0/Avion.Interop.dll     the P/Invoke wrapper (.NET 6/8, .NET Framework 4.6.1+, Unity)
     runtimes/win-x64/native/avion.dll        the engine (copied to .NET outputs by the SDK's RID resolution)
     build/native/Avion.Sdk.targets           native C/C++ projects: include path, avion.lib, avion.dll copied
     build/native/include/media/c_api.h, build/native/lib/x64/avion.lib
     src/Avion.Interop.cs                     the wrapper's source (Unity, or to read)
     LICENSE, LICENSING.md, README.md, [Content_Types].xml, _rels/.rels
Reproducible (fixed timestamps, sorted entries). Prints the package's path."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
VERSION = "0.1.0"
STAMP = (2026, 11, 4, 0, 0, 0)

NUSPEC = f"""<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://schemas.microsoft.com/packaging/2013/05/nuspec.xsd">
  <metadata>
    <id>Avion.Sdk</id>
    <version>{VERSION}</version>
    <title>AVION media engine SDK</title>
    <authors>Alexandre Heymann</authors>
    <owners>Alexandre Heymann</owners>
    <license type="expression">AGPL-3.0-only</license>
    <requireLicenseAcceptance>false</requireLicenseAcceptance>
    <repository type="git" url="https://github.com/5645hm-a11y/avion" />
    <projectUrl>https://5645hm-a11y.github.io/avion/</projectUrl>
    <readme>README.md</readme>
    <description>AVION, a zero-dependency media engine for Windows (C99 SDK, API 14): GPU playback and hardware decoding (H.264, HEVC
8/10-bit), shared Direct3D 11/12 texture output in 8-bit, 10-bit PQ or FP16 scRGB, perception telemetry (time to contact, objects,
remote vibrometry and pulse rate, a virtual IMU), zero-copy tensor export, an event camera and bitstream perception. Dual-licensed:
GNU AGPLv3, or a commercial licence from the author for closed-source software, devices and services (LICENSING.md).</description>
    <tags>video media player engine d3d12 hdr gpu decoding native interop</tags>
    <dependencies>
      <group targetFramework=".NETStandard2.0" />
    </dependencies>
  </metadata>
</package>
"""
CONTENT_TYPES = """<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml" />
  <Default Extension="nuspec" ContentType="application/octet" />
  <Default Extension="dll" ContentType="application/octet" />
  <Default Extension="lib" ContentType="application/octet" />
  <Default Extension="h" ContentType="application/octet" />
  <Default Extension="cs" ContentType="application/octet" />
  <Default Extension="targets" ContentType="application/octet" />
  <Default Extension="md" ContentType="application/octet" />
</Types>
"""
RELS = """<?xml version="1.0" encoding="utf-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Type="http://schemas.microsoft.com/packaging/2010/07/manifest" Target="/Avion.Sdk.nuspec" Id="R0" />
</Relationships>
"""
CSPROJ = """<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>netstandard2.0</TargetFramework>
    <AssemblyName>Avion.Interop</AssemblyName>
    <RootNamespace>Avion</RootNamespace>
    <LangVersion>8.0</LangVersion>
    <Nullable>disable</Nullable>
    <Version>""" + VERSION + """</Version>
    <Authors>Alexandre Heymann</Authors>
    <Deterministic>true</Deterministic>
    <TreatWarningsAsErrors>true</TreatWarningsAsErrors>
  </PropertyGroup>
</Project>
"""


def build_interop(scratch):
    project = scratch / "interop"
    project.mkdir(parents=True)
    (project / "Avion.Interop.csproj").write_text(CSPROJ, encoding="utf-8")
    shutil.copy(HERE / "Avion.Interop.cs", project / "Avion.Interop.cs")
    built = subprocess.run(["dotnet", "build", str(project / "Avion.Interop.csproj"), "-c", "Release", "-nologo", "-v", "q", "-o", str(project / "out")],
                           capture_output=True, text=True)
    if built.returncode != 0:
        sys.stderr.write(built.stdout + built.stderr)
        raise SystemExit("dotnet build of Avion.Interop failed")
    return (project / "out" / "Avion.Interop.dll").read_bytes()


def main(argv):
    if len(argv) != 4:
        print(__doc__); return 2
    dll, lib, out = pathlib.Path(argv[1]), pathlib.Path(argv[2]), pathlib.Path(argv[3])
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="avion_nuget_"))
    try:
        interop = build_interop(scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    readme = (HERE / "README.md").read_bytes()
    files = {
        "Avion.Sdk.nuspec": NUSPEC.encode("utf-8"),
        "[Content_Types].xml": CONTENT_TYPES.encode("utf-8"),
        "_rels/.rels": RELS.encode("utf-8"),
        "lib/netstandard2.0/Avion.Interop.dll": interop,
        "runtimes/win-x64/native/avion.dll": dll.read_bytes(),
        "build/native/Avion.Sdk.targets": (HERE / "Avion.Sdk.targets").read_bytes(),
        "build/native/include/media/c_api.h": (ROOT / "include" / "media" / "c_api.h").read_bytes(),
        "build/native/lib/x64/avion.lib": lib.read_bytes(),
        "src/Avion.Interop.cs": (HERE / "Avion.Interop.cs").read_bytes(),
        "LICENSE": (ROOT / "LICENSE").read_bytes(),
        "LICENSING.md": (ROOT / "LICENSING.md").read_bytes(),
        "README.md": readme,
    }
    out.mkdir(parents=True, exist_ok=True)
    package = out / f"Avion.Sdk.{VERSION}.nupkg"
    with zipfile.ZipFile(package, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(files):
            entry = zipfile.ZipInfo(name, STAMP); entry.compress_type = zipfile.ZIP_DEFLATED; entry.external_attr = 0o644 << 16
            z.writestr(entry, files[name])
    print(package)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

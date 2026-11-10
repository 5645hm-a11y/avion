"""Stage the AVION 0.1 evaluation bundle, zip it deterministically and write the SHA-256 delivery manifest.
   python tools/release/build_bundle.py <repo root>   (after tools/site/build_site.py --docs; before the full site build)"""
import hashlib, json, pathlib, shutil, sys, zipfile

repo = pathlib.Path(sys.argv[1])
share = repo / "artifacts" / "share"
stage = share / "AVION-0.1-Evaluation"
release = repo / "build" / "Release"

copies = {
    "include/media/c_api.h": repo / "include/media/c_api.h",
    "docs/sdk.md": repo / "docs/sdk.md",
    "bin/media_player_demo.exe": release / "media_player_demo.exe",
    "samples/playback_fixture.mp4": repo / "build/playback_fixture.mp4",
    # The Release suite's log, saved when that run ended (LastTest.log is the latest run's, which may be Debug's).
    "evidence/ctest-release.log": repo / "build/Testing/release-LastTest.log",
    # The head-to-head: report, raw lines, summary, the harness and every script that made them.
    "benchmarks/benchmarks.md": repo / "docs/benchmarks.md",
    "benchmarks/raw/linux_results.jsonl": repo / "build/bench/linux_results.jsonl",
    "benchmarks/raw/windows_results.jsonl": repo / "build/bench/windows_results.jsonl",
    "benchmarks/raw/summary.json": repo / "build/bench/summary.json",
    "benchmarks/tools/bench_headtohead.cpp": repo / "tools/bench_headtohead.cpp",
    "benchmarks/tools/malloc_count.c": repo / "tools/bench/malloc_count.c",
    "benchmarks/tools/opencv_baseline.cpp": repo / "tools/bench/opencv_baseline.cpp",
    "benchmarks/tools/run_linux.sh": repo / "tools/bench/run_linux.sh",
    "benchmarks/tools/run_windows.py": repo / "tools/bench/run_windows.py",
    "benchmarks/tools/report.py": repo / "tools/bench/report.py",
    "benchmarks/tools/thread_cpu.py": repo / "tools/bench/thread_cpu.py",
    "bin/media_bench_headtohead.exe": release / "media_bench_headtohead.exe",
    # C99 samples on the public header only: the site's quickstart and the program that made the site's showcase.
    "samples/quickstart.c": repo / "tools/showcase/quickstart.c",
    "samples/avion_showcase.c": repo / "tools/showcase/avion_showcase.c",
    "bin/media_quickstart.exe": release / "media_quickstart.exe",
    "bin/media_showcase.exe": release / "media_showcase.exe",
    # API 12: the SDK as a DLL for any language, and players that embed it with their own interface.
    "bin/avion.dll": release / "avion.dll",
    "samples/README.md": repo / "samples/README.md",
    "samples/csharp/AvionPlayer/AvionPlayer.csproj": repo / "samples/csharp/AvionPlayer/AvionPlayer.csproj",
    "samples/csharp/AvionPlayer/Avion.cs": repo / "samples/csharp/AvionPlayer/Avion.cs",
    "samples/csharp/AvionPlayer/PlayerForm.cs": repo / "samples/csharp/AvionPlayer/PlayerForm.cs",
    "samples/csharp/AvionPlayer/Program.cs": repo / "samples/csharp/AvionPlayer/Program.cs",
    "samples/python/avion_player.py": repo / "samples/python/avion_player.py",
    # API 14: the licence, and the developer packages (tools/python, tools/nuget; cmake --target avion_packages).
    "LICENSE": repo / "LICENSE",
    "LICENSING.md": repo / "LICENSING.md",
    "packages/avion-0.1.0-py3-none-win_amd64.whl": repo / "artifacts/packages/avion-0.1.0-py3-none-win_amd64.whl",
    "packages/Avion.Sdk.0.1.0.nupkg": repo / "artifacts/packages/Avion.Sdk.0.1.0.nupkg",
}
for lib in sorted(release.glob("*.lib")):
    copies[f"lib/{lib.name}"] = lib
# Earlier names of files this list no longer stages.
for rel in ("evidence/ctest-release-210of210.log", "evidence/verification-api11.md", "evidence/verification-api12.md", "evidence/verification-api13.md",
            "evidence/verification-api14.md"):
    (stage / rel).unlink(missing_ok=True)
for rel, src in copies.items():
    dst = stage / rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)

# The verification of this build, as handed to hosts (artifacts/share/HOST_HANDOFF_API17.md): what changed, how it was tested,
# what failed on the way.
shutil.copyfile(share / "HOST_HANDOFF_API17.md", stage / "evidence" / "verification.md")

def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()

files = sorted(p for p in stage.rglob("*") if p.is_file())
archive = share / "AVION-0.1-Evaluation.zip"
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for p in files:
        info = zipfile.ZipInfo("AVION-0.1-Evaluation/" + p.relative_to(stage).as_posix(), date_time=(2026, 11, 2, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        z.writestr(info, p.read_bytes())

su = json.loads((repo / "build/bench/summary.json").read_text(encoding="utf-8"))["suites"]
manifest = {
    "product": "AVION 0.1",
    "description": "Deterministic bionic perception, vision and media kernel (C99 SDK / C++20 / Direct3D 12)",
    "sdk_api_version": 17,
    "license": "AGPL-3.0-only OR LicenseRef-AVION-Commercial (LICENSING.md; the AGPLv3 text in LICENSE); source: https://github.com/5645hm-a11y/avion",
    "packages": ["packages/avion-0.1.0-py3-none-win_amd64.whl", "packages/Avion.Sdk.0.1.0.nupkg"],
    "build": {"compiler": "MSVC x64, Visual Studio 18 2026", "configuration": "Release", "flags": "/W4 /WX /O2 /Ob2 /DNDEBUG"},
    "verification": {
        # From the runs' own logs (tools/bench/report.py's "suites").
        **{f"msvc_{c}": f"{su[c]['passed']}/{su[c]['total']} passed; failed: {', '.join(su[c]['failed']) or 'none'}; skipped: {', '.join(su[c]['skipped']) or 'none'} "
                        f"(CTest serial, {su[c]['seconds']:,.0f} s)" for c in ("release", "debug")},
        "gcc": f"{su['gcc']['passed']}/{su['gcc']['total']} result lines passed: portable suites x (-O2, ASan+UBSan), GCC 13.3, WSL Ubuntu 24.04",
        "evidence": ["evidence/ctest-release.log", "evidence/verification.md"],
        "head_to_head": ["benchmarks/benchmarks.md", "benchmarks/raw/summary.json"],
    },
    "integrity_note": "SHA-256 digests for integrity checking. This manifest is not cryptographically signed.",
    "archive": {"path": archive.name, "bytes": archive.stat().st_size, "sha256": digest(archive)},
    "files": [{"path": p.relative_to(stage).as_posix(), "bytes": p.stat().st_size, "sha256": digest(p)} for p in files],
}
(share / "AVION-0.1-delivery-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
print(archive.stat().st_size, manifest["archive"]["sha256"], len(files))

"""Matroska / WebM fixtures for the API 17 demuxer (2026-11-05): what ffmpeg writes for the codec pairs people meet, with subtitles
and chapters, and stream-copy remuxes of the large MP4 fixtures (the same pictures and sound in another container).

    python -I tools/realworld/make_mkv_fixtures.py <assets/test_fixtures> [--large]

Writes <fixtures>/mkv/. ffmpeg (with libx264, libx265, libsvtav1, libopus) is a development tool here only; the engine never uses it.
Run it where ffmpeg is (the WSL distribution "Ubuntu" on the development machine).
"""
import os, subprocess, sys

SRT = """1
00:00:00,500 --> 00:00:02,000
First line of the film

2
00:00:02,500 --> 00:00:04,000
Second line, with <i>style</i>

3
00:00:04,500 --> 00:00:05,800
Third line
"""
ASS = """[Script Info]
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.50,0:00:02.00,Default,,0,0,0,,{\\i1}First{\\i0} line\\Nof the film
Dialogue: 0,0:00:02.50,0:00:04.00,Default,,0,0,0,,Second line
"""
CHAPTERS = """;FFMETADATA1
[CHAPTER]
TIMEBASE=1/1000
START=0
END=2000
title=Opening
[CHAPTER]
TIMEBASE=1/1000
START=2000
END=4000
title=Middle part
[CHAPTER]
TIMEBASE=1/1000
START=4000
END=6000
title=Finale
"""

PICTURE = ["-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:duration=6"]
SOUND = ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=6", "-ac", "2"]


def run(args, out):
    r = subprocess.run(["ffmpeg", "-y", "-v", "error", *args, out], capture_output=True, text=True)
    print(("ok   " if r.returncode == 0 else "FAIL ") + os.path.basename(out) + ("" if r.returncode == 0 else ": " + r.stderr.strip()[-300:]))


def main():
    root = sys.argv[1]
    out = os.path.join(root, "mkv")
    os.makedirs(out, exist_ok=True)
    srt, ass, chapters = (os.path.join(out, n) for n in ("cues.srt", "cues.ass", "chapters.txt"))
    for path, text in ((srt, SRT), (ass, ASS), (chapters, CHAPTERS)):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    x264 = ["-c:v", "libx264", "-preset", "veryfast", "-bf", "3", "-g", "60", "-pix_fmt", "yuv420p"]
    aac = ["-c:a", "aac", "-b:a", "128k"]
    opus = ["-c:a", "libopus", "-b:a", "96k"]
    # H.264 (B pictures) + AAC + SRT subtitles + chapters: the everyday .mkv.
    run([*PICTURE, *SOUND, "-i", srt, "-f", "ffmetadata", "-i", chapters, "-map", "0:v", "-map", "1:a", "-map", "2:s", "-map_metadata", "3",
         "-map_chapters", "3", *x264, *aac, "-c:s", "srt", "-metadata:s:s:0", "language=eng", "-metadata:s:a:0", "language=eng"],
        os.path.join(out, "h264_aac_srt_chapters.mkv"))
    # The same with an ASS subtitle track.
    run([*PICTURE, *SOUND, "-i", ass, "-map", "0:v", "-map", "1:a", "-map", "2:s", *x264, *aac, "-c:s", "ass"], os.path.join(out, "h264_aac_ass.mkv"))
    # HEVC Main + AAC.
    run([*PICTURE, *SOUND, "-c:v", "libx265", "-preset", "veryfast", "-x265-params", "log-level=error:bframes=3", "-pix_fmt", "yuv420p", "-tag:v", "hvc1", *aac],
        os.path.join(out, "hevc_aac.mkv"))
    # HEVC Main 10.
    run([*PICTURE, *SOUND, "-c:v", "libx265", "-preset", "veryfast", "-x265-params", "log-level=error", "-pix_fmt", "yuv420p10le", *aac],
        os.path.join(out, "hevc10_aac.mkv"))
    # H.264 + Opus (Matroska; WebM allows VP8/VP9/AV1 only).
    run([*PICTURE, *SOUND, *x264, *opus], os.path.join(out, "h264_opus.mkv"))
    # AV1 + Opus: the everyday .webm (YouTube, browsers' MediaRecorder).
    av1 = ["-c:v", "libsvtav1", "-preset", "10", "-g", "60", "-pix_fmt", "yuv420p"]
    run([*PICTURE, *SOUND, *av1, *opus], os.path.join(out, "av1_opus.webm"))
    # AV1 + AAC in Matroska.
    run([*PICTURE, *SOUND, *av1, *aac], os.path.join(out, "av1_aac.mkv"))
    # A live-style WebM: unknown-size segment and clusters, no Cues (what MediaRecorder writes).
    run([*PICTURE, *SOUND, *av1, *opus, "-live", "1", "-f", "webm"], os.path.join(out, "av1_opus_live.webm"))
    # media_http_mkv_seek_test (2026-11-09): 10 minutes of 720p30 H.264 with B pictures (198 MB, Cues at the end, as ffmpeg writes
    # them), mild temporal noise so the stream is not trivially small. A few minutes to encode.
    run(["-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=600", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=600",
         "-vf", "noise=alls=8:allf=t", "-c:v", "libx264", "-preset", "veryfast", "-bf", "3", "-g", "60", "-pix_fmt", "yuv420p",
         "-b:v", "2500k", "-maxrate", "3000k", "-bufsize", "6000k", *aac, "-ac", "2", "-metadata:s:a:0", "language=eng"],
        os.path.join(out, "h264_bframes_720p_10m.mkv"))
    if "--large" in sys.argv:
        # Stream copies of the large fixtures: 1.4 GB of 4K60 HEVC, and the 45-minute 4K60 AV1 clock.
        run(["-i", os.path.join(root, "big_buck_bunny_4k60.mp4"), "-c", "copy"], os.path.join(out, "big_buck_bunny_4k60.mkv"))
        run(["-i", os.path.join(root, "av_sync_clock_45m.mp4"), "-c", "copy"], os.path.join(out, "av_sync_clock_45m.mkv"))


if __name__ == "__main__":
    main()

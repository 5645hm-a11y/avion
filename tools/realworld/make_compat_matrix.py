"""Write a matrix of everyday MP4 variants (what phones, editors, YouTube downloads and ffmpeg defaults produce) for probe_open.py.

    python -I tools/realworld/make_compat_matrix.py <out_dir>

ffmpeg is a development tool here only. Each file is 4 s of test pattern and tone at 640x360 unless its name says otherwise.
"""
import os, struct, subprocess, sys

SRC_V = ["-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:duration=4"]
SRC_A = ["-f", "lavfi", "-i", "sine=frequency=440:duration=4:sample_rate=48000"]
SRC_A6 = ["-f", "lavfi", "-i", "sine=frequency=440:duration=4:sample_rate=48000,pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0"]
X264 = ["-c:v", "libx264"]
AAC = ["-c:a", "aac", "-b:a", "128k"]

VARIANTS = {
    "x264_high_default": SRC_V + SRC_A + X264 + AAC,
    "x264_faststart": SRC_V + SRC_A + X264 + AAC + ["-movflags", "+faststart"],
    "x264_baseline": SRC_V + SRC_A + X264 + ["-profile:v", "baseline"] + AAC,
    "x264_main": SRC_V + SRC_A + X264 + ["-profile:v", "main"] + AAC,
    "x264_no_editlist": SRC_V + SRC_A + X264 + AAC + ["-use_editlist", "0"],
    "x264_negative_cts": SRC_V + SRC_A + X264 + AAC + ["-movflags", "+negative_cts_offsets"],
    "x264_fragmented": SRC_V + SRC_A + X264 + AAC + ["-movflags", "+frag_keyframe+empty_moov+default_base_moof"],
    "x264_cmaf": SRC_V + SRC_A + X264 + AAC + ["-movflags", "+cmaf+frag_keyframe"],
    "x264_avc3": SRC_V + SRC_A + X264 + AAC + ["-tag:v", "avc3"],
    "x264_high10": SRC_V + SRC_A + X264 + ["-pix_fmt", "yuv420p10le"] + AAC,
    "x264_high422": SRC_V + SRC_A + X264 + ["-pix_fmt", "yuv422p"] + AAC,
    "x264_high444": SRC_V + SRC_A + X264 + ["-pix_fmt", "yuv444p"] + AAC,
    "x264_fullrange_yuvj": SRC_V + SRC_A + X264 + ["-pix_fmt", "yuvj420p"] + AAC,
    "x264_interlaced_tff": SRC_V + SRC_A + X264 + ["-flags", "+ildct+ilme", "-x264opts", "tff=1"] + AAC,
    "x264_weightp_8refs_b16": SRC_V + SRC_A + X264 + ["-preset", "veryslow"] + AAC,
    "x264_vfr_60": SRC_V[:3] + ["testsrc2=size=640x360:rate=60:duration=4"] + SRC_A + X264 + ["-fps_mode", "vfr"] + AAC,
    "x264_odd_size_642x362": SRC_V[:3] + ["testsrc2=size=642x362:rate=30:duration=4"] + SRC_A + X264 + AAC,
    "x264_aac_he": SRC_V + SRC_A + X264 + ["-c:a", "aac", "-profile:a", "aac_he", "-b:a", "48k"],
    "x264_aac_51": SRC_V + SRC_A6 + X264 + AAC,
    "x264_aac_44100": SRC_V + ["-f", "lavfi", "-i", "sine=frequency=440:duration=4:sample_rate=44100"] + X264 + AAC,
    "x264_two_audio": SRC_V + SRC_A + SRC_A + X264 + AAC + ["-map", "0:v", "-map", "1:a", "-map", "2:a"],
    "x264_mov_text_subs": SRC_V + SRC_A + ["-f", "lavfi", "-i", "anullsrc=duration=4"] + X264 + AAC + ["-map", "0:v", "-map", "1:a"],
    "x264_ac3_audio": SRC_V + SRC_A + X264 + ["-c:a", "ac3"],
    "x264_mp3_audio": SRC_V + SRC_A + X264 + ["-c:a", "libmp3lame"],
    "x264_no_audio": SRC_V + X264 + ["-an"],
    "x264_rotated_90": SRC_V + SRC_A + X264 + AAC + ["-display_rotation:v:0", "90"],
    "x264_timecode_track": SRC_V + SRC_A + X264 + AAC + ["-timecode", "01:00:00:00"],
    "x264_mov_container": SRC_V + SRC_A + X264 + AAC,  # .mov, written below
    "x264_brand_isom_m4v": SRC_V + SRC_A + X264 + AAC,  # .m4v
    "x264_start_offset": ["-itsoffset", "0.5"] + SRC_V + SRC_A + X264 + AAC,
    # A user's file from mpv's encoder, made again synthetically (2026-11-10): 25 fps H.264 with B-pictures in a 24000 timescale,
    # 44.1 kHz mono AAC, moov after mdat, and a last video sample of no duration (stts ..., (1, 0)), which the engine once refused
    # ("stage 4"). ffmpeg writes the stream; zero_last_delta() then gives the last sample its zero duration.
    "stts_last_zero": ["-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=4",
                       "-f", "lavfi", "-i", "sine=frequency=440:duration=4:sample_rate=44100"] + X264 + ["-bf", "2", "-video_track_timescale", "24000"]
                      + AAC + ["-ac", "1", "-map_metadata", "-1", "-fflags", "+bitexact", "-flags:v", "+bitexact", "-flags:a", "+bitexact"],
}
POST = {"stts_last_zero": "zero_last_delta"}
EXT = {"x264_mov_container": ".mov", "x264_brand_isom_m4v": ".m4v"}


CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"edts"}


def boxes(data, start, end):
    """(type, offset, size) of each box in [start, end), descending into the sample-table containers."""
    p = start
    while p + 8 <= end:
        size, kind = struct.unpack(">I4s", data[p:p + 8])
        if size < 8:
            raise ValueError("unsupported box size")
        yield kind, p, size
        if kind in CONTAINERS:
            yield from boxes(data, p + 8, p + size)
        p += size


def zero_last_delta(path):
    """Split the video track's last stts run so its final sample lasts 0, as mpv's file does. moov must follow mdat (ffmpeg's
    default), so growing moov by 8 bytes moves no sample: only the enclosing boxes' sizes change. mdhd, tkhd and the edit list
    lose that sample's duration, as in the original file."""
    data = bytearray(open(path, "rb").read())
    found = list(boxes(data, 0, len(data)))
    moov = next(b for b in found if b[0] == b"moov")
    if moov[1] < next(b for b in found if b[0] == b"mdat")[1]:
        raise ValueError("moov before mdat")
    movie_scale = struct.unpack(">I", data[moov[1] + 8 + 20:moov[1] + 8 + 24])[0]  # mvhd version 0: timescale at +20.
    traks = [b for b in found if b[0] == b"trak"]
    for trak in traks:
        inner = [b for b in found if trak[1] < b[1] < trak[1] + trak[2]]
        hdlr = next(b for b in inner if b[0] == b"hdlr")
        if bytes(data[hdlr[1] + 16:hdlr[1] + 20]) != b"vide":
            continue
        stts = next(b for b in inner if b[0] == b"stts")
        count = struct.unpack(">I", data[stts[1] + 12:stts[1] + 16])[0]
        last = stts[1] + 16 + 8 * (count - 1)
        run, delta = struct.unpack(">II", data[last:last + 8])
        if delta == 0 or run < 2:
            raise ValueError("unexpected stts")
        mdhd = next(b for b in inner if b[0] == b"mdhd")
        media_scale, media_duration = struct.unpack(">II", data[mdhd[1] + 20:mdhd[1] + 28])
        total = sum(struct.unpack(">II", data[stts[1] + 16 + 8 * i:stts[1] + 24 + 8 * i])[0] * struct.unpack(">II", data[stts[1] + 16 + 8 * i:stts[1] + 24 + 8 * i])[1]
                    for i in range(count)) - delta  # The deltas' sum once the last one is 0, as in the reported file.
        data[mdhd[1] + 24:mdhd[1] + 28] = struct.pack(">I", total)
        shrink = delta * movie_scale // media_scale
        tkhd = next(b for b in inner if b[0] == b"tkhd")
        duration = struct.unpack(">I", data[tkhd[1] + 28:tkhd[1] + 32])[0]
        data[tkhd[1] + 28:tkhd[1] + 32] = struct.pack(">I", duration - shrink)
        for elst in (b for b in inner if b[0] == b"elst"):
            segment = struct.unpack(">I", data[elst[1] + 16:elst[1] + 20])[0]
            data[elst[1] + 16:elst[1] + 20] = struct.pack(">I", segment - shrink)
        # The new entry (1, 0) after the shortened run; every enclosing box grows by 8 bytes.
        data[last:last + 8] = struct.pack(">II", run - 1, delta) + struct.pack(">II", 1, 0)
        data[stts[1] + 12:stts[1] + 16] = struct.pack(">I", count + 1)
        for kind, offset, size in found:
            if offset <= stts[1] < offset + size and kind in CONTAINERS | {b"stts"}:
                data[offset:offset + 4] = struct.pack(">I", size + 8)
        open(path, "wb").write(data)
        return
    raise ValueError("no video track")


def main(out):
    os.makedirs(out, exist_ok=True)
    for name, args in VARIANTS.items():
        path = os.path.join(out, name + EXT.get(name, ".mp4"))
        if os.path.exists(path):
            continue
        r = subprocess.run(["ffmpeg", "-y", "-v", "error", *args, "-shortest", path], capture_output=True, text=True)
        if r.returncode == 0 and name in POST:
            globals()[POST[name]](path)
        print(("ok   " if r.returncode == 0 else "FAIL ") + name + ("" if r.returncode == 0 else ": " + r.stderr.strip().splitlines()[-1]))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])

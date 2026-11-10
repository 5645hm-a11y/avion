"""Prepare a TUM VI sequence (CC BY 4.0, https://cvg.cit.tum.de/data/datasets/visual-inertial-dataset) for the real-world checks.

    python -I tools/realworld/prep_tumvi.py <dataset-roomN_512_16.tar> <out_dir>

Writes in <out_dir>:
  cam0.gray        8-bit luma, 512x512, all cam0 pictures back to back (the 16-bit PNGs converted by ffmpeg)
  cam0_crfNN.mp4   the same pictures as H.264 (yuv420p, 20 fps), for the compression sweep and the engine's own decode
  cam0_crfNN.gray  those files decoded back to luma
  imu0.csv, cam0_times.csv, mocap0.csv   copied from the sequence (nanosecond timestamps)
ffmpeg is a development tool here, never a dependency of the engine.
"""
import os, shutil, subprocess, sys, tarfile

W = H = 512
CRFS = (18, 28, 35)


def run(*args):
    subprocess.run(args, check=True)


def main(tar_path, out_dir):
    tar_path, out_dir = os.path.abspath(tar_path), os.path.abspath(out_dir)  # ffmpeg's concat list resolves paths against its own folder
    os.makedirs(out_dir, exist_ok=True)
    root = os.path.join(out_dir, "src")
    if not os.path.isdir(root):
        with tarfile.open(tar_path) as t:
            t.extractall(root, filter="data")
    seq = next(os.path.join(root, d) for d in os.listdir(root))
    mav = os.path.join(seq, "mav0")
    cam = os.path.join(mav, "cam0")
    rows = [l.strip().split(",") for l in open(os.path.join(cam, "data.csv")) if not l.startswith("#")]
    names = [r[1] for r in rows]
    with open(os.path.join(out_dir, "cam0_times.csv"), "w") as f:
        f.write("index,t_ns\n")
        for i, r in enumerate(rows):
            f.write(f"{i},{r[0]}\n")
    lst = os.path.join(out_dir, "cam0_list.txt")
    with open(lst, "w", encoding="utf-8") as f:
        for n in names:
            f.write(f"file '{os.path.join(cam, 'data', n).replace(os.sep, '/')}'\nduration 0.05\n")
    gray = os.path.join(out_dir, "cam0.gray")
    run("ffmpeg", "-y", "-v", "error", "-f", "concat", "-safe", "0", "-i", lst, "-fps_mode", "passthrough", "-vf", "format=gray", "-f", "rawvideo", gray)
    n = os.path.getsize(gray) // (W * H)
    assert n == len(names), (n, len(names))
    for crf in CRFS:
        mp4 = os.path.join(out_dir, f"cam0_crf{crf}.mp4")
        run("ffmpeg", "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "gray", "-s", f"{W}x{H}", "-r", "20", "-i", gray,
            "-c:v", "libx264", "-preset", "medium", "-crf", str(crf), "-pix_fmt", "yuv420p", mp4)
        run("ffmpeg", "-y", "-v", "error", "-i", mp4, "-vf", "format=gray", "-f", "rawvideo", os.path.join(out_dir, f"cam0_crf{crf}.gray"))
    shutil.copy(os.path.join(mav, "imu0", "data.csv"), os.path.join(out_dir, "imu0.csv"))
    mocap = os.path.join(mav, "mocap0", "data.csv")
    if os.path.exists(mocap):
        shutil.copy(mocap, os.path.join(out_dir, "mocap0.csv"))
    print(f"{n} pictures, {len(CRFS)} H.264 variants -> {out_dir}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])

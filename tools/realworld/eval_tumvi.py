"""Compare the virtual IMU with the real gyroscope on prepared TUM VI sequences.

    python -I tools/realworld/eval_tumvi.py <eval_vimu.exe> <fit_dir> <test_dir> [<test_dir> ...] [--variant raw|crf18|crf28|crf35]

For every picture pair the gyroscope (200 Hz) is integrated between the two exposures (bias taken from the still start of the
sequence) and the virtual IMU's (dx, dy, turn) are fitted to it by a robust 3x3 map on the *fit* sequence. The map absorbs the
camera/IMU rotation and the focal length; it is then applied unchanged to the *test* sequences (same rig), so the numbers are not
a fit to what they judge. Reported per test sequence: the correlation, the residual rms in pixels, the share of pictures with
a valid motion, and the rms of the motion itself (what a perfect answer would have to explain).
Translation of the handheld rig is parallax the gyroscope cannot explain; it counts against the virtual IMU here.
"""
import os, subprocess, sys
import numpy as np


def load_gyro(d):
    a = np.loadtxt(os.path.join(d, "imu0.csv"), delimiter=",", comments="#")
    return a[:, 0] * 1e-9, a[:, 1:4]


def load_times(d):
    return np.loadtxt(os.path.join(d, "cam0_times.csv"), delimiter=",", skiprows=1)[:, 1] * 1e-9


def pair_rotation(t_img, t_imu, w):
    """Integrated gyro rate between consecutive pictures, bias removed: (n-1, 3) radians."""
    still = t_imu < t_imu[0] + 1.5
    bias = np.median(w[still], axis=0)
    w = w - bias
    cum = np.concatenate([np.zeros((1, 3)), np.cumsum(0.5 * (w[1:] + w[:-1]) * np.diff(t_imu)[:, None], axis=0)])
    at = np.stack([np.interp(t_img, t_imu, cum[:, k]) for k in range(3)], axis=1)
    return np.diff(at, axis=0)


def run_tool(exe, d, variant):
    gray = os.path.join(d, "cam0.gray" if variant == "raw" else f"cam0_{variant}.gray")
    out = os.path.join(d, f"vimu_{variant}.csv")
    subprocess.run([exe, gray, "512", "512", "106", out], check=True, capture_output=True)
    a = np.loadtxt(out, delimiter=",", skiprows=1)
    return a  # index, valid, dx, dy, turn, tiles, inliers, residual


def pairs(exe, d, variant):
    t_img, (t_imu, w) = load_times(d), load_gyro(d)
    rot = pair_rotation(t_img, t_imu, w)               # picture i-1 -> i, rows 0..n-2 are pictures 1..n-1
    v = run_tool(exe, d, variant)[1:]
    ok = (v[:, 1] > 0) & (np.abs(rot).max(axis=1) < 0.5)
    meas = v[:, 2:5]
    return rot, meas, ok, v[:, 1].mean()


def load_calibration(d):
    """(R_cam_imu 3x3, focal px/rad) from the sequence's own dso/camchain.yaml (cam0, equidistant): no fitting."""
    import glob, re
    path = glob.glob(os.path.join(d, "src", "*", "dso", "camchain.yaml"))[0]
    text = open(path).read().split("cam1:")[0]
    rows = re.findall(r"- \[(-?[\d.e-]+), (-?[\d.e-]+), (-?[\d.e-]+), (-?[\d.e-]+)\]", text)[:3]
    r = np.array([[float(v) for v in row[:3]] for row in rows])
    f = float(re.search(r"intrinsics: \[([\d.]+), ([\d.]+)", text).group(1))
    return r, f


def calibrated(exe, d, variant):
    """Zero free parameters: the gyro rotated into the camera by the factory calibration and scaled by its focal length.
    Only the three signs (the picture's axis conventions) are chosen, by the sign of the correlation, never the size."""
    rot, meas, ok, valid = pairs(exe, d, variant)
    r, f = load_calibration(d)
    wc = rot @ r.T                      # camera-frame rotation per pair (x right, y down, z forward)
    cand = np.stack([f * wc[:, 1], f * wc[:, 0], wc[:, 2]], axis=1)   # yaw -> dx, pitch -> dy, roll -> turn
    sign = np.sign([np.corrcoef(cand[ok, k], meas[ok, k])[0, 1] for k in range(3)])
    pred = cand * sign
    return meas, pred, ok, valid, f, sign


def report_calibrated(exe, d, variant):
    meas, pred, ok, valid, f, sign = calibrated(exe, d, variant)
    res = meas[ok][:, :2] - pred[ok][:, :2]
    mot = meas[ok][:, :2]
    slope = [float((meas[ok][:, k] * pred[ok][:, k]).sum() / (pred[ok][:, k] ** 2).sum()) for k in range(3)]
    corr = [np.corrcoef(meas[ok][:, k], pred[ok][:, k])[0, 1] for k in range(3)]
    print(f"{os.path.basename(d):10s} f={f:.1f} px/rad  residual rms {np.sqrt((res**2).sum(1).mean()):.3f} px of {np.sqrt((mot**2).sum(1).mean()):.2f} px  "
          f"explained {1 - (res**2).sum() / (mot**2).sum():.3f}  measured/gyro gain x {slope[0]:.3f} y {slope[1]:.3f} roll {slope[2]:.3f}  "
          f"corr x {corr[0]:.3f} y {corr[1]:.3f} roll {corr[2]:.3f}  valid {valid:.2f}")


def robust_fit(x, y, iters=8):
    w = np.ones(len(x))
    for _ in range(iters):
        m, *_ = np.linalg.lstsq(x * w[:, None], y * w[:, None], rcond=None)
        r = np.linalg.norm(y - x @ m, axis=1)
        s = 1.4826 * np.median(r) + 1e-9
        w = 1.0 / np.maximum(1.0, r / (2.5 * s))
    return m


def main():
    args = [a for a in sys.argv[1:]]
    variant = "raw"
    if "--variant" in args:
        i = args.index("--variant"); variant = args[i + 1]; del args[i:i + 2]
    exe, fit, tests = args[0], args[1], args[2:]
    print(f"Calibrated, no free parameter (factory T_cam_imu and focal length; variant {variant}):")
    for d in dict.fromkeys([fit] + tests):
        report_calibrated(exe, d, variant)
    print()
    rot, meas, ok, _ = pairs(exe, fit, variant)
    m = robust_fit(rot[ok], meas[ok])
    print(f"variant {variant}; fit on {os.path.basename(fit)} ({ok.sum()} pairs); map (px per rad) rows = gyro x,y,z:")
    print(np.array2string(m, precision=1, suppress_small=True))
    # Honest check on the fit sequence itself: fit on its first half, judge its second half.
    half = len(rot) // 2
    rot_a, meas_a, ok_a, _ = pairs(exe, fit, variant)
    ok_first = ok_a.copy(); ok_first[half:] = False
    ok_second = ok_a.copy(); ok_second[:half] = False
    m_half = robust_fit(rot_a[ok_first], meas_a[ok_first])
    res_h = meas_a[ok_second][:, :2] - (rot_a[ok_second] @ m_half)[:, :2]
    mot_h = meas_a[ok_second][:, :2]
    print(f"first-half fit on the second half: motion rms {np.sqrt((mot_h**2).sum(1).mean()):.3f} px, residual rms "
          f"{np.sqrt((res_h**2).sum(1).mean()):.3f} px, explained {1 - (res_h**2).sum() / (mot_h**2).sum():.3f}")
    print(f"{'sequence':28s} {'pairs':>6s} {'valid':>6s} {'motion rms':>11s} {'residual rms':>13s} {'explained':>10s} {'corr dx':>8s} {'corr dy':>8s}")
    for d in dict.fromkeys([fit] + tests):
        rot, meas, ok, valid = pairs(exe, d, variant)
        pred = rot[ok] @ m
        res = meas[ok][:, :2] - pred[:, :2]
        mot = meas[ok][:, :2]
        corr = [np.corrcoef(meas[ok][:, k], pred[:, k])[0, 1] for k in (0, 1)]
        ex = 1 - (res ** 2).sum() / max((mot ** 2).sum(), 1e-12)
        tag = os.path.basename(d) + (" (fit)" if d == fit else "")
        print(f"{tag:28s} {len(rot):6d} {valid:6.2f} {np.sqrt((mot**2).sum(1).mean()):11.3f} {np.sqrt((res**2).sum(1).mean()):13.3f} {ex:10.3f} {corr[0]:8.3f} {corr[1]:8.3f}")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    main()

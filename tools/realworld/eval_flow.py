"""Tile-flow accuracy against dense ground truth, next to its cost: AVION vs sparse pyramidal Lucas-Kanade on the same points.

    python -I tools/realworld/eval_flow.py <media_eval_flow.exe> middlebury <dir with other-gt-flow + other-color-twoframes extracted>
    python -I tools/realworld/eval_flow.py <media_eval_flow.exe> sintel <MPI-Sintel-complete extracted> [clean|final]

The comparison a reviewer asked for (2026-11-04): AVION outputs one vector per tile (16 x 16 tiles a picture, each 1/16 of it a
side), so the fair baseline is OpenCV's calcOpticalFlowPyrLK at the same 256 tile centres, not dense DIS. Dense DIS is kept as an
accuracy reference (its field averaged over each tile). Ground truth per tile: the mean of the dense ground truth over the tile's
non-occluded pixels (tiles with under half of them visible are skipped). Error: endpoint error (EPE), pixels per frame.
Every method is reported on (a) the tiles it calls valid, with its coverage, and (b) the tiles all methods call valid.
Times: one core (cv2.setNumThreads(1)), per picture pair; AVION's from its own Push timing (it also computes TTC, saliency and
objects in that call, so it is an upper bound for the flow alone).
"""
import glob, os, subprocess, sys, tempfile, time
import numpy as np
import cv2

T = 16  # tiles a side
AVION_ONLY = "--avion-only" in sys.argv  # Iterating on AVION: the baselines' rows copy AVION tile flow (not measured).
if AVION_ONLY: sys.argv.remove("--avion-only")


def read_flo(path):
    with open(path, "rb") as f:
        tag = np.frombuffer(f.read(4), np.float32)[0]
        assert abs(tag - 202021.25) < 1e-3, path
        w, h = np.frombuffer(f.read(8), np.int32)
        return np.frombuffer(f.read(), np.float32).reshape(h, w, 2).copy()


def gray(path):
    # cv2.imread cannot open a non-ASCII path on Windows (the project lives under 'Développement'): decode from bytes.
    img = cv2.imdecode(np.fromfile(path, np.uint8), cv2.IMREAD_GRAYSCALE)
    assert img is not None, path
    return img


def middlebury(root):
    for gt in sorted(glob.glob(os.path.join(root, "**", "flow10.flo"), recursive=True)):
        name = os.path.basename(os.path.dirname(gt))
        frames = glob.glob(os.path.join(root, "**", name, "frame1[01].png"), recursive=True)
        frames = sorted(frames)
        if len(frames) != 2:
            continue
        flow = read_flo(gt)
        valid = np.all(np.abs(flow) < 1e9, axis=2)   # Middlebury marks unknown flow with 1e9
        yield name, [gray(p) for p in frames], [flow], [valid], 1.0 / 30


def sintel(root, pass_name="clean"):
    base = os.path.join(root, "training")
    for seq in sorted(os.listdir(os.path.join(base, pass_name))):
        frames = sorted(glob.glob(os.path.join(base, pass_name, seq, "*.png")))
        flows = sorted(glob.glob(os.path.join(base, "flow", seq, "*.flo")))
        occ = sorted(glob.glob(os.path.join(base, "occlusions", seq, "*.png")))
        gts = [read_flo(p) for p in flows]
        vis = [(gray(p) == 0) for p in occ] if occ else [np.ones(g.shape[:2], bool) for g in gts]
        yield f"{pass_name}/{seq}", [gray(p) for p in frames], gts, vis, 1.0 / 24


def tile_bounds(w, h):
    xs = [round(i * w / T) for i in range(T + 1)]
    ys = [round(j * h / T) for j in range(T + 1)]
    return xs, ys


SPREAD = []  # Per tile with ground truth, pair by pair: the mean distance of its pixels' flow from the tile's mean (2026-11-05).


def tile_truth(flow, vis, w, h):
    xs, ys = tile_bounds(w, h)
    gt = np.zeros((T * T, 2)); ok = np.zeros(T * T, bool); spread = np.zeros(T * T)
    for j in range(T):
        for i in range(T):
            f = flow[ys[j]:ys[j + 1], xs[i]:xs[i + 1]]; v = vis[ys[j]:ys[j + 1], xs[i]:xs[i + 1]]
            if v.mean() >= 0.5:
                m = f[v].mean(axis=0)
                gt[j * T + i] = m; ok[j * T + i] = True; spread[j * T + i] = np.linalg.norm(f[v] - m, axis=1).mean()
    SPREAD.append(spread)
    return gt, ok


def centres(w, h):
    xs, ys = tile_bounds(w, h)
    return np.array([[(xs[i] + xs[i + 1]) / 2, (ys[j] + ys[j + 1]) / 2] for j in range(T) for i in range(T)], np.float32)


def run_avion(exe, frames, dt, refine=False, pyramid=False):
    h, w = frames[0].shape
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, "p.gray"); out = os.path.join(tmp, "f.bin"); tim = os.path.join(tmp, "t.csv")
        np.stack(frames).tofile(raw)
        args = [exe, raw, str(w), str(h), str(len(frames)), str(int(round(dt * 1e7))), out, tim] + (["refine"] if refine else []) + (["pyramid"] if pyramid else [])
        subprocess.run(args, check=True)
        rec = np.fromfile(out, np.float32).reshape(len(frames) - 1, T * T, 3)
        times = np.loadtxt(tim, delimiter=",", ndmin=2)[1:].sum(axis=1)   # Push + Refine, per picture after the first
    return [(r[:, 1:3].astype(np.float64), r[:, 0] > 0) for r in rec], times


LK_SETTINGS = {
    "LK 15x15, 3 levels": dict(winSize=(15, 15), maxLevel=3),
    "LK 21x21, 4 levels": dict(winSize=(21, 21), maxLevel=4),
    "LK 31x31, 5 levels": dict(winSize=(31, 31), maxLevel=5),
}


def run_lk(frames, params):
    h, w = frames[0].shape
    p0 = centres(w, h).reshape(-1, 1, 2)
    crit = (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 30, 0.01)
    res, times = [], []
    for a, b in zip(frames[:-1], frames[1:]):
        t = time.perf_counter()
        p1, st, err = cv2.calcOpticalFlowPyrLK(a, b, p0, None, criteria=crit, **params)
        times.append((time.perf_counter() - t) * 1e6)
        res.append(((p1 - p0).reshape(-1, 2).astype(np.float64), st.reshape(-1) > 0))
    return res, np.array(times)


def run_dis(frames, preset):
    h, w = frames[0].shape
    dis = cv2.DISOpticalFlow_create(preset)
    xs, ys = tile_bounds(w, h)
    res, times = [], []
    for a, b in zip(frames[:-1], frames[1:]):
        t = time.perf_counter()
        f = dis.calc(a, b, None)
        times.append((time.perf_counter() - t) * 1e6)
        v = np.array([f[ys[j]:ys[j + 1], xs[i]:xs[i + 1]].reshape(-1, 2).mean(0) for j in range(T) for i in range(T)])
        res.append((v, np.ones(T * T, bool)))
    return res, np.array(times)


def main():
    cv2.setNumThreads(1)
    exe, kind, root = sys.argv[1], sys.argv[2], sys.argv[3]
    seqs = middlebury(root) if kind == "middlebury" else sintel(root, sys.argv[4] if len(sys.argv) > 4 else "clean")
    # 2026-11-05 (API 17): the coarse level (the default) against the single-level search of 2026-10-29.
    methods = ["AVION tile flow", "AVION + FlowRefiner", "AVION + coarse level", "AVION + coarse level + FlowRefiner"] + list(LK_SETTINGS) + ["DIS ultrafast (dense, tile mean)", "DIS medium (dense, tile mean)", "zero motion"]
    store = {m: {"err": [], "ok": [], "time": []} for m in methods}
    gt_all, gtok_all = [], []
    for name, frames, flows, vis, dt in seqs:
        h, w = frames[0].shape
        n = min(len(frames) - 1, len(flows))
        frames = frames[:n + 1]
        gts = [tile_truth(flows[k], vis[k], w, h) for k in range(n)]
        outs = {"AVION tile flow": run_avion(exe, frames, dt), "AVION + FlowRefiner": run_avion(exe, frames, dt, refine=True),
                "AVION + coarse level": run_avion(exe, frames, dt, pyramid=True),
                "AVION + coarse level + FlowRefiner": run_avion(exe, frames, dt, refine=True, pyramid=True)}
        for m, p in LK_SETTINGS.items():
            outs[m] = run_lk(frames, p) if not AVION_ONLY else outs["AVION tile flow"]
        outs["DIS ultrafast (dense, tile mean)"] = run_dis(frames, cv2.DISOPTICAL_FLOW_PRESET_ULTRAFAST) if not AVION_ONLY else outs["AVION tile flow"]
        outs["DIS medium (dense, tile mean)"] = run_dis(frames, cv2.DISOPTICAL_FLOW_PRESET_MEDIUM) if not AVION_ONLY else outs["AVION tile flow"]
        outs["zero motion"] = ([(np.zeros((T * T, 2)), np.ones(T * T, bool))] * n, np.zeros(n))
        for k in range(n):
            gt, gok = gts[k]
            gt_all.append(gt); gtok_all.append(gok)
            for m in methods:
                v, ok = outs[m][0][k]
                store[m]["err"].append(np.linalg.norm(v - gt, axis=1)); store[m]["ok"].append(ok)
        for m in methods:
            store[m]["time"].extend(list(outs[m][1]))
        per = []
        for m in ("AVION tile flow", "AVION + coarse level", "AVION + FlowRefiner", "LK 21x21, 4 levels"):
            e = np.concatenate([np.linalg.norm(outs[m][0][k][0] - gts[k][0], axis=1) for k in range(n)])
            ok = np.concatenate([outs[m][0][k][1] & gts[k][1] for k in range(n)])
            per.append(f"{m} {np.mean(e[ok]):.2f}")
        print(f"  {name}: {n} pairs, {w}x{h}, EPE " + ", ".join(per), file=sys.stderr)
    gt = np.concatenate(gt_all); gok = np.concatenate(gtok_all)
    mag = np.linalg.norm(gt, axis=1)
    common = gok.copy()
    for m in methods:
        common &= np.concatenate(store[m]["ok"])
    print(f"\n{kind}: {len(gt_all)} picture pairs, {gok.sum()} tiles with ground truth, {common.sum()} valid for every method")
    print(f"ground-truth tile motion: median {np.median(mag[gok]):.2f} px, p90 {np.percentile(mag[gok], 90):.2f} px, max {mag[gok].max():.1f} px\n")
    bins = [(0, 2), (2, 8), (8, 32), (32, 1e9)]
    spread = np.concatenate(SPREAD)
    uniform = spread < 0.2 * np.maximum(mag, 1e-9)  # One motion across the tile: no object edge inside it.
    head = f"{'method':34s} {'coverage':>8s} {'EPE own':>8s} {'EPE common':>10s} {'p90 common':>10s} " + " ".join(f"{f'{a:g}-{b:g}px':>9s}".replace("-1e+09", "+") for a, b in bins) + f" {'32+ unif':>9s} {'cov 32+':>8s} {'us/pair':>8s}"
    print(head)
    for m in methods:
        e = np.concatenate(store[m]["err"]); ok = np.concatenate(store[m]["ok"]) & gok
        cov = ok.sum() / gok.sum()
        by = []
        for a, b in bins:
            s = common & (mag >= a) & (mag < b)
            by.append(f"{np.mean(e[s]):9.2f}" if s.sum() else f"{'-':>9s}")
        s32 = common & (mag >= 32) & uniform
        by.append(f"{np.mean(e[s32]):9.2f}" if s32.sum() else f"{'-':>9s}")
        big = gok & (mag >= 32)
        by.append(f"{(ok & big).sum() / max(big.sum(), 1):8.1%}")
        t = np.array(store[m]["time"]); tm = f"{np.median(t):8.0f}" if t.any() else f"{'-':>8s}"
        print(f"{m:34s} {cov:8.1%} {np.mean(e[ok]):8.2f} {np.mean(e[common]):10.2f} {np.percentile(e[common], 90):10.2f} " + " ".join(by) + f" {tm}")
    big = gok & (mag >= 32)
    print(f"\n32+ px tiles: {big.sum()}, uniform ones (spread under 20 % of the motion): {(big & uniform).sum()}, common and uniform: {(common & (mag >= 32) & uniform).sum()}")
    print("EPE: mean endpoint error per tile, px/frame. Bins by ground-truth motion over the common tiles. us/pair: median, one core.")


if __name__ == "__main__":
    main()

"""Why the coarse level does not solve fast motion (2026-11-05; docs/benchmarks.md, "Why the coarse level fails").

    python -I tools/realworld/trace_flow.py <media_flow_trace.exe> <MPI-Sintel-complete extracted> [clean|final] [--sequences N]

Runs media_flow_trace (media_eval_flow with Perception's search traced tile by tile, the coarse level on) over the Sintel training
sequences and, for every main tile whose ground truth is one uniform motion (spread under a fifth of it) of 32+ px a frame, follows
its search stage by stage: which baseline was tried (Perception measures over the newest picture at least kBaseline = 90 ms old:
at Sintel's 24 fps that is three frames back), where the truth lies in fine and coarse cells over that baseline, what the fine
window found (and whether it "settled", which skips the coarse level), whether the coarse level ran, what it answered and why it
refused, and what the re-centred window did. The counts say where the motion is lost.
"""
import os, subprocess, sys, tempfile
from collections import Counter
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eval_flow as ef  # noqa: E402  (its readers and tile truth)

CELLS, TILE_CELLS, RATIO, COARSE_SEARCH, SEARCH = 128, 8, 4, 5, 6
REASONS = {0: "match", 1: "texture", 2: "window off the grid", 3: "no clear minimum", 4: "minimum on the window's edge", 5: "not a strict minimum"}


def parse(path):
    """picture -> tile -> list of attempts (dicts), in the order tried."""
    out = {}
    pic = None
    with open(path) as f:
        for line in f:
            if line.startswith("P "):
                pic = int(line.split()[1]); out[pic] = {}; continue
            if not line.startswith("A "):
                continue
            tok = line.split()
            a = dict(bx=int(tok[1]), by=int(tok[2]), age=int(tok[3]), sec=float(tok[4]), cx=int(tok[5]), cy=int(tok[6]),
                     fine=int(tok[7]), fdx=float(tok[8]), fdy=float(tok[9]), edge=int(tok[10]), cost=int(tok[11]), settled=int(tok[12]))
            i = 13
            a["coarse"] = None; a["far"] = None
            if i < len(tok) and tok[i] == "C":
                a["coarse"] = dict(found=int(tok[i + 1]), qx=float(tok[i + 2]), qy=float(tok[i + 3]), reason=int(tok[i + 4]),
                                   best=int(tok[i + 5]), costs=int(tok[i + 6]), on=int(tok[i + 7]), bi=int(tok[i + 8]), bj=int(tok[i + 9]))
                i += 10
            if i < len(tok) and tok[i] == "F":
                a["far"] = dict(matched=int(tok[i + 1]), fx=float(tok[i + 2]), fy=float(tok[i + 3]), cost=int(tok[i + 4]), taken=int(tok[i + 5]))
                i += 6
            assert tok[i] == "R", line
            a["matched"] = int(tok[i + 1]); a["dx"] = float(tok[i + 2]); a["dy"] = float(tok[i + 3])
            t = (a["by"] // TILE_CELLS) * ef.T + a["bx"] // TILE_CELLS
            out[pic].setdefault(t, []).append(a)
    return out


def main():
    exe, root = sys.argv[1], sys.argv[2]
    pass_name = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else "clean"
    limit = int(sys.argv[sys.argv.index("--sequences") + 1]) if "--sequences" in sys.argv else 10 ** 9
    stages = Counter(); reasons = Counter(); n_tiles = 0
    reach = Counter(); detail = []
    first_age = Counter(); by_reach = Counter()
    for s, (name, frames, flows, vis, dt) in enumerate(ef.sintel(root, pass_name)):
        if s >= limit:
            break
        h, w = frames[0].shape
        n = min(len(frames) - 1, len(flows)); frames = frames[:n + 1]
        gts = [ef.tile_truth(flows[k], vis[k], w, h) for k in range(n)]
        spreads = ef.SPREAD[-n:]
        cell_w, cell_h = w / CELLS, h / CELLS
        with tempfile.TemporaryDirectory() as tmp:
            raw = os.path.join(tmp, "p.gray"); out = os.path.join(tmp, "f.bin"); trace = os.path.join(tmp, "trace.txt")
            np.stack(frames).tofile(raw)
            env = dict(os.environ, MEDIA_FLOW_TRACE=trace)
            subprocess.run([exe, raw, str(w), str(h), str(len(frames)), str(int(round(dt * 1e7))), out, os.path.join(tmp, "t.csv"), "pyramid"], check=True, env=env)
            rec = np.fromfile(out, np.float32).reshape(len(frames) - 1, ef.T * ef.T, 3)
            tr = parse(trace)
        for k in range(n):
            gt, ok = gts[k]
            mag = np.linalg.norm(gt, axis=1)
            for t in np.nonzero(ok & (mag >= 32) & (spreads[k] < 0.2 * np.maximum(mag, 1e-9)))[0]:
                attempts = tr.get(k + 1, {}).get(int(t), [])
                if not attempts:
                    continue
                n_tiles += 1
                valid, vx, vy = rec[k, t]
                err = float(np.hypot(vx - gt[t, 0], vy - gt[t, 1])) if valid > 0 else None
                first_age[attempts[0]["age"]] += 1
                # The truth over the first baseline tried, in fine and coarse cells (Perception's displacement: then -> now).
                a0 = attempts[0]
                frames_back = a0["sec"] / dt
                ex, ey = gt[t, 0] * frames_back / cell_w, gt[t, 1] * frames_back / cell_h
                cex, cey = ex / RATIO, ey / RATIO
                inside = abs(cex) <= COARSE_SEARCH - 1 + 0.5 and abs(cey) <= COARSE_SEARCH - 1 + 0.5  # the coarse window's interior
                reach["coarse window holds the truth" if inside else "truth beyond the coarse window"] += 1
                # Which stage decided the outcome (the first baseline's attempt).
                if valid > 0 and err is not None and err < 8:
                    stage = "solved (error < 8 px)"
                elif a0["settled"]:
                    stage = "fine window settled on a wrong match: coarse level never ran" if valid > 0 else "fine window settled, then refused"
                elif a0["coarse"] is None:
                    stage = "coarse level not run (other)"
                elif not a0["coarse"]["found"]:
                    stage = "coarse level refused"
                    reasons[REASONS[a0["coarse"]["reason"]]] += 1
                else:
                    q = a0["coarse"]
                    right = abs(q["qx"] - ex) <= 2 * RATIO and abs(q["qy"] - ey) <= 2 * RATIO
                    if not right:
                        stage = "coarse level answered wrong"
                    elif a0["far"] is None:
                        stage = "coarse right but within kCoarseRecentre: not re-searched"
                    elif not a0["far"]["matched"]:
                        stage = "coarse right, re-centred window found nothing"
                    elif not a0["far"]["taken"]:
                        stage = "coarse right, re-centred match refused (cost / agreement)"
                    else:
                        stage = "coarse right and taken, final still wrong"
                stages[stage] += 1
                by_reach[("inside" if inside else "beyond", stage)] += 1
                if len(detail) < 40 and stage != "solved (error < 8 px)":
                    detail.append((name, k, int(t), round(float(gt[t, 0]), 1), round(float(gt[t, 1]), 1), a0["age"], round(ex, 1), round(ey, 1), stage,
                                   a0["coarse"] and (a0["coarse"]["found"], a0["coarse"]["qx"], a0["coarse"]["qy"], REASONS[a0["coarse"]["reason"]],
                                                     a0["coarse"]["best"] * a0["coarse"]["on"] / max(a0["coarse"]["costs"], 1))))
        print(f"  {name}: {n_tiles} uniform 32+ px tiles so far", file=sys.stderr)
    print(f"\nUniform tiles of 32+ px a frame traced: {n_tiles}")
    print("First baseline tried (pictures back):", dict(first_age))
    print("Where the truth lies over that baseline:", dict(reach))
    print("\nOutcome by stage:")
    for k, v in stages.most_common():
        print(f"  {v:6d} {v / max(n_tiles, 1):6.1%}  {k}")
    print("\nOutcome by stage, truth inside / beyond the coarse window over the first baseline:")
    for (where, k), v in sorted(by_reach.items(), key=lambda x: (x[0][0], -x[1])):
        print(f"  {where:6s} {v:6d}  {k}")
    print("\nCoarse refusals by reason:")
    for k, v in reasons.most_common():
        print(f"  {v:6d}  {k}")
    print("\nExamples (sequence, pair, tile, truth px/frame, age, truth in cells over the baseline, stage, coarse (found, qx, qy, reason, best*n/sum)):")
    for d in detail:
        print("  ", d)


if __name__ == "__main__":
    main()

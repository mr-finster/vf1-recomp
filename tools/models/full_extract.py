#!/usr/bin/env python3
# The character exactly as the game draws it: player 1's record parts plus
# every other fighter object on player 1's body (hair strands, braids,
# ribbons, sashes), from a VF_DUMP_ALL dump; shadows (flat on the floor) and
# the opponent are left out.   full_extract.py dump_all.json out.json
import json, sys, numpy as np
sys.path.insert(0, __import__("os").path.dirname(__file__))
import ball_joints as bj
P = [json.loads(l) for l in open(sys.argv[1])]
rec = [p for p in P if not p["tag"].startswith("x")]
# Against the same character (Jacky vs Jacky) the opponent's copies of shared
# models carry the same tags. At round start player 1 stands on the left
# (camera x < 0): of each tag's copies keep the leftmost.
best = {}
for p in rec:
    cx = bj.pose(p["m"], p["p"])[:, 0].mean()
    if p["tag"] not in best or cx < best[p["tag"]][0]: best[p["tag"]] = (cx, p)
keep = [best[t][1] for t in dict.fromkeys(p["tag"] for p in rec)]
R = np.vstack([bj.pose(p["m"], p["p"]) for p in keep]); lo, hi = R.min(0), R.max(0)
extra = 0
for p in P:
    if not p["tag"].startswith("x"): continue
    W = bj.pose(p["m"], p["p"]); c = W.mean(0)
    if np.ptp(W[:, 1]) < 0.005 and abs(W[:, 1].mean() - lo[1]) < 0.05: continue   # a shadow
    if np.all(c > lo - 0.05) and np.all(c < hi + 0.05):
        q = dict(p); q["tag"] = "s" + p["tag"][1:]; keep.append(q); extra += 1
with open(sys.argv[2], "w") as f:
    for p in keep: f.write(json.dumps(p) + "\n")
print(f"{len(keep) - extra} body parts + {extra} strands/details")

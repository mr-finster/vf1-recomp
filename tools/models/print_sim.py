#!/usr/bin/env python3
# Simulate printing a statue on a Bambu-type FDM printer (0.4 mm nozzle, PLA):
# slice into layers, find what needs support (overhangs past the threshold and
# floating islands), build the supports a slicer would place (normal supports,
# straight down to the plate or the model, with a 0.2 mm gap), and estimate
# filament, time and balance.   print_sim.py statue_on_base.stl OUTDIR NAME
import sys, os, json, numpy as np, manifold3d as m3
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_print as cp, hidden_joints as hj
from render_studio import render
LAYER = 0.16; ANGLE = 55.0                  # a wall can lean this far from vertical unsupported
PLA = 1.24                                  # g/cm³
stl, out, name = sys.argv[1], sys.argv[2], sys.argv[3]; os.makedirs(out, exist_ok=True)
M, T = cp.load(stl); T = T.astype(float)
bb = M.bounding_box(); top = bb[5]
reach = LAYER * np.tan(np.radians(ANGLE))
# --- slice -----------------------------------------------------------------
prev = None; unsup = []; islands = []; areas = []; first_area = 0.0
zs = np.arange(LAYER / 2, top, LAYER)
for z in zs:
    cs = M.slice(float(z)); a = cs.area(); areas.append(a)
    if prev is None:
        first_area = a
    else:
        u = cs - prev.offset(reach, m3.JoinType.Round)
        ua = u.area()
        if ua > 0.05: unsup.append((z, u))
        for comp in cs.decompose():
            if (comp ^ prev).area() < 1e-3 and comp.area() > 0.05: islands.append((z, comp.area()))
    prev = cs
# --- supports: unsupported regions, extruded straight down, minus the model --
cols = []
band = {}
for z, u in unsup:                            # merge per 1 mm band to keep it light
    k = int(z); band[k] = band.get(k, m3.CrossSection()) + u
for k, u in band.items():
    if u.area() < 0.3: continue
    h = float(k + 1)
    cols.append(u.extrude(h))
sup = m3.Manifold.batch_boolean(cols, m3.OpType.Add) if cols else m3.Manifold()
gap = M.minkowski_sum(m3.Manifold.sphere(0.2, 8)) if not sup.is_empty() else None
if gap is not None: sup = sup - gap
sup_vol = sup.volume() if not sup.is_empty() else 0.0
# --- estimates -------------------------------------------------------------
V = M.volume(); A = M.surface_area()
walls = min(V, A * 2 * 0.42)                   # 2 perimeters of 0.42 mm
model_ext = walls + 0.15 * max(0.0, V - walls) # + 15% infill
support_ext = 0.15 * sup_vol                   # supports print ~15% dense
grams = (model_ext + support_ext) / 1000 * PLA
t_lo = (model_ext + support_ext) / 9.0 / 3600 + len(zs) * 2.5 / 3600
t_hi = (model_ext + support_ext) / 5.0 / 3600 + len(zs) * 4.0 / 3600
# --- balance: centre of mass vs the base ----------------------------------
v0, v1, v2 = T[:, 0], T[:, 1], T[:, 2]
vol6 = np.einsum("ij,ij->i", v0, np.cross(v1, v2)); com = ((v0 + v1 + v2) * vol6[:, None]).sum(0) / (4 * vol6.sum())
cs0 = M.slice(0.05); pts = np.vstack([np.asarray(p) for p in cs0.to_polygons()])
c0 = pts.mean(0); base_r = np.linalg.norm(pts - c0, axis=1).max()
off = float(np.linalg.norm(com[:2] - c0))
res = dict(name=name, layers=len(zs), height_mm=round(top, 1), first_layer_mm2=round(first_area),
           overhang_area_mm2=round(sum(u.area() for _, u in unsup), 1), islands=len(islands),
           island_heights_mm=sorted({round(z) for z, _ in islands})[:12],
           support_volume_cm3=round(sup_vol / 1000, 1), filament_g=round(grams), time_h=[round(t_lo, 1), round(t_hi, 1)],
           com_offset_mm=round(off, 1), base_radius_mm=round(base_r, 1), stable=bool(off < 0.6 * base_r))
json.dump(res, open(f"{out}/print_sim.json", "w"), indent=1); print(json.dumps(res))
# --- pictures: the model with what needs support in red, and the supports ---
nz = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0]); nz = nz[:, 2] / (np.linalg.norm(nz, axis=1) + 1e-12)
zc = T[:, :, 2].mean(1)
need = (nz < -np.cos(np.radians(90 - ANGLE))) & (zc > 0.5)
col = np.where(need[:, None], [0.85, 0.25, 0.2], [0.62, 0.64, 0.68])
if not sup.is_empty():
    sv, sf = hj.mesh_of(sup); ST = sv[sf].astype(float)
    allT = np.vstack([T, ST]); allC = np.vstack([col, np.tile([0.95, 0.78, 0.45], (len(ST), 1))])
else:
    allT, allC = T, col
render(allT, f"{out}/{name}_print_supports.png", yaw=0.55, pitch=0.18, res=900, colors=allC)
render(T, f"{out}/{name}_print_overhangs.png", yaw=0.55, pitch=0.18, res=900, colors=col)
hj.bj.write_stl(f"{out}/{name}_supports_preview.stl", [hj.mesh_of(sup)] if not sup.is_empty() else [])

# tools/gpu/msl_bind.py in.spv out.spv reflect.json (tools/gpu/shaders.sh): a compute shader's SPIR-V Binding
# decorations made SDL_GPU's Metal indices ([[buffer]]: uniform buffers, read-only storage buffers, read-write storage
# buffers; [[texture]]: sampled, read-only storage, read-write storage textures), so SPIRV-Cross's --msl-decoration-binding
# gives the Metal shader the places SDL_GPU binds them
import json, struct, sys
src, dst, refl = sys.argv[1:4]
d = json.load(open(refl))
w = list(struct.unpack('<%dI' % (len(open(src, 'rb').read()) // 4), open(src, 'rb').read()))
res = []   # (kind, set, binding)
for r in d.get('ubos', []): res.append(('ubo', r['set'], r['binding']))
for r in d.get('ssbos', []): res.append(('ro' if r.get('readonly') else 'rw', r['set'], r['binding']))
for r in d.get('images', []): res.append(('rwt', r['set'], r['binding']))
n = {k: sum(1 for x in res if x[0] == k) for k in ('ubo', 'ro', 'rw', 'rwt')}
if any(x[0] == 'ro' and x[1] != 0 for x in res) or any(x[0] in ('rw', 'rwt') and x[1] != 1 for x in res) or any(x[0] == 'ubo' and x[1] != 2 for x in res):
    sys.exit('unexpected sets: ' + repr(res))
def new(kind, b):
    if kind == 'ubo': return b
    if kind == 'ro': return n['ubo'] + b                         # (t[n], space0: no textures before them here)
    if kind == 'rwt': return b                                    # (u[n], space1: read-write textures first)
    if kind == 'rw': return n['ubo'] + n['ro'] + (b - n['rwt'])   # (then the read-write buffers)
# ids' sets and bindings from the decorations
sets, binds = {}, {}
i = 5
while i < len(w):
    op, cnt = w[i] & 0xffff, w[i] >> 16
    if op == 71 and cnt >= 4:   # OpDecorate id decoration literal
        if w[i + 2] == 34: sets[w[i + 1]] = w[i + 3]
        if w[i + 2] == 33: binds[w[i + 1]] = i + 3
    i += cnt
kind_of = {(s, b): k for k, s, b in res}
for vid, at in binds.items():
    key = (sets.get(vid), w[at])
    if key not in kind_of: sys.exit('no resource for %r' % (key,))
    w[at] = new(kind_of[key], w[at])
open(dst, 'wb').write(struct.pack('<%dI' % len(w), *w))
print(src, n)

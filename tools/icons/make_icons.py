# The select icons for the secret characters (Cheats > Secret character), from renders of their heads
# made by make.sh: the mapping from a render to an icon is fitted on Akira's (his real icon against his
# head rendered the same way), then used for all of them; the head's tones matched to his icon's,
# 12 colours by k-means, in his icon's frame. Run by make.sh: make_icons.py WORK fit|make OUT
import numpy as np, itertools, json, os, sys
os.chdir(sys.argv[1]); sys.argv = sys.argv[1:]
OUT = sys.argv[2] if len(sys.argv) > 2 else 'icons'
from PIL import Image
S='.'
BOX=(562,95,942,495)
real=np.asarray(Image.open('o/icon/akira.png').convert('RGB')).astype(int)
plate=np.asarray(Image.open('o/ic_plate/p.png').convert('RGB').crop(BOX)).astype(int)
IX0,IY0,IW,IH=4,2,48,56          # the picture inside the frame
BLUE=np.array([0,0,255])
def layer(a):
    im=np.asarray(Image.open(f'o/ic_{a}/p.png').convert('RGB').crop(BOX)).astype(int)
    m=(np.abs(im-plate).sum(2)>40).astype(np.uint8)*255
    return Image.fromarray(np.dstack([im,m]).astype(np.uint8),'RGBA')
def place(lay,s,ox,oy):
    # the 380x400 layer scaled by s (at 4x the icon), put at (ox,oy) in a 4x interior, then boxed down
    W,H=IW*4,IH*4
    big=Image.new('RGBA',(W,H),(0,0,255,255))
    l=lay.resize((round(380*s),round(400*s)),Image.LANCZOS)
    big.alpha_composite(l,(round(ox),round(oy)))
    return np.asarray(big.convert('RGB').resize((IW,IH),Image.BOX)).astype(int)
def kmeans(pic,k,it=30):
    # colours by k-means (weighted by how many pixels): median cut merged skin into its neighbours
    px=pic.reshape(-1,3).astype(float); rng=np.random.default_rng(1)
    c=px[rng.choice(len(px),k,replace=False)]
    # start from far-apart colours (k-means++)
    c=[px[0]]
    for _ in range(k-1):
        d=np.min([((px-cc)**2).sum(1) for cc in c],0); c.append(px[np.argmax(d)])
    c=np.array(c)
    for _ in range(it):
        lab=((px[:,None,:]-c[None])**2).sum(2).argmin(1)
        for j in range(k):
            if (lab==j).any(): c[j]=px[lab==j].mean(0)
    return c[lab].round().clip(0,255).astype(np.uint8).reshape(pic.shape)
def lum(x): return (x*[0.3,0.59,0.11]).sum(-1)
if sys.argv[1]=='fit':
    lay=layer(0); tgt=lum(real[IY0:IY0+IH,IX0:IX0+IW])
    best=None
    for s in np.arange(0.70,1.60,0.04):
        for ox in range(-320,40,8):
            for oy in range(-320,40,8):
                e=np.abs(lum(place(lay,s,ox,oy))-tgt).mean()
                if best is None or e<best[0]: best=(e,s,ox,oy)
    e,s,ox,oy=best
    for s2 in np.arange(s-0.02,s+0.021,0.005):
        for ox2 in range(ox-6,ox+7,2):
            for oy2 in range(oy-6,oy+7,2):
                e2=np.abs(lum(place(lay,s2,ox2,oy2))-tgt).mean()
                if e2<best[0]: best=(e2,s2,ox2,oy2)
    print(best); json.dump(best[1:],open('fit.json','w'))
else:
    s,ox,oy=json.load(open('fit.json'))
    # the real icons' tones: ours matched to Akira's (mean and spread of the head's pixels, per channel)
    ours=place(layer(0),s,ox,oy).astype(float); r=real[IY0:IY0+IH,IX0:IX0+IW].astype(float)
    ho=~(ours==BLUE).all(-1); hr=~((r[...,0]<20)&(r[...,1]<20)&(r[...,2]>200))
    M0=lum(ours[ho]).mean(); M1=lum(r[hr]).mean(); K=lum(r[hr]).std()/lum(ours[ho]).std(); print('tone',M0,M1,K)
    names={0:'akira',2:'jeff',3:'takeru',4:'rekka',5:'score',6:'aeon',7:'dural'}
    for a,n in names.items():
        pic=place(layer(a),s,ox,oy).astype(float)
        bg=(pic==BLUE).all(-1)
        pic=np.where(bg[...,None],pic,np.clip(pic*(M1/M0),0,255)).astype(np.uint8)
        q=Image.fromarray(kmeans(pic,12))
        out=real.copy().astype(np.uint8)
        out[IY0:IY0+IH,IX0:IX0+IW]=np.asarray(q)
        # the frame's right blue column stays as VF's
        Image.fromarray(out).save(f'{OUT}/{n}.png')
        print(n, len(set(map(tuple,out.reshape(-1,3)))), 'colours')

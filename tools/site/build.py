#!/usr/bin/env python3
"""Builds the download page into build/site, ready for Cloudflare Pages. Deploy from the repo root (the
production branch is on-device-recomp; any other branch is only a preview):
  npx wrangler pages deploy build/site --project-name vf1-recomp --branch on-device-recomp

The page (tools/site/index.html) holds nothing from the game. The screenshots, the video and its
poster are made from it, so they live only in build/site (never in git):
  build/site/shots/NN.webp (the trailer itself is on YouTube, in the Watch popup),
  media/screen.mp4 and screen.webp (the online trailer's fights at 640x360, 30 fps, no sound track: the cabinet's
  screen, and its poster; tools/site/media.py makes them from the trailer),
  media/poster.jpg, media/og.jpg
The downloads are copied from build/release, build/win and build/deck, and each must be the build of
this version (its program carries the version string exactly): a test build left there stops it.

The referrer count: the page pings /_refhit on its own origin (the one-line script at the end of the
page; no comment there, the page has none), and a Pages Function there passes the reading on to the
collector, server-side, so nothing in the page names another host. The Function is written to
functions/_refhit.js at the repo root, outside build/site: wrangler takes functions/ from the folder
it runs in, and outside the deployed folder its source is never served as a file. It does nothing
until the project has the REF_KEY secret (wrangler pages secret put REF_KEY --project-name vf1-recomp).
The collector's address is not in the source: it is read from .refhit-url at the repo root (untracked),
or VF_REF_URL; without it, no Function is written.

  tools/site/build.py [--url https://vf1-recomp.pages.dev] [--version (default: the latest tag)]
"""
import argparse, re, datetime, json, os, shutil, subprocess, sys

REFHIT = '''// The page pings /_refhit on its OWN origin; this passes the reading to the collector
// server-side. Nothing in the markup points at another host, so the sites stay unlinkable.
// Only the canonical hostname reports — preview deployments are ignored, so a *.pages.dev
// preview URL can never pollute the real counts.
const SITE = "vf1-recomp.pages.dev";
export async function onRequest(ctx) {
  const url = new URL(ctx.request.url);
  const res = new Response(null, { status: 204, headers: { "cache-control": "no-store" } });
  if (url.hostname !== SITE || !ctx.env.REF_KEY) return res;
  const q = new URLSearchParams({ s: SITE, r: url.searchParams.get("r") || "", k: ctx.env.REF_KEY });
  ctx.waitUntil(fetch(%s + q).catch(() => {}));
  return res;
}
'''
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'build', 'site')

# the carousel, in order: file in build/site/shots, and what it shows
SHOTS = [
    ('00.webp', 'Title screen in widescreen'),
    ('01.webp', 'Player select in widescreen'),
    ('02.webp', 'Jacky and Sarah on the beach stage'),
    ('03.webp', 'Kage and Lau on the night city stage'),
    ('04.webp', 'Lau kicks Akira by the lake'),
    ('05.webp', 'A replay: Wolf and Jeffry close up'),
    ('06.webp', 'Sarah and Wolf at dusk'),
    ('07.webp', 'Pai and Kage in the desert'),
    ('08.webp', 'A replay: Pai and Jeffry'),
    ('09.webp', 'Lau wins by K.O.'),
    ('10.webp', 'Sarah knees Wolf at sunset'),
    ('11.webp', 'A replay from above'),
]


def size(path):
    mb = os.path.getsize(path) / 1e6
    return f'{mb:.1f} MB'


def carries(package, version):
    """Whether a package's program has exactly this version (VF_VERSION is a C string in it)."""
    import tarfile, zipfile
    want = version.encode() + b'\0'
    if package.endswith('.zip'):
        with zipfile.ZipFile(package) as z:
            return any(want in z.read(i) for i in z.infolist() if not i.is_dir() and i.file_size < 64 << 20)
    with tarfile.open(package) as t:
        return any(want in t.extractfile(m).read() for m in t.getmembers() if m.isfile() and m.size < 64 << 20)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='https://vf1-recomp.pages.dev')
    ap.add_argument('--version', default=None)
    args = ap.parse_args()
    if not args.version:   # the latest release tag, never a made-up number
        args.version = subprocess.run(['git', 'describe', '--tags', '--abbrev=0'], cwd=ROOT, capture_output=True,
                                      text=True, check=True).stdout.strip()

    for f, _ in SHOTS:
        if not os.path.exists(os.path.join(OUT, 'shots', f)):
            raise SystemExit(f'missing build/site/shots/{f}')
    for f in ('screen.mp4', 'screen.webp', 'poster.jpg', 'og.jpg'):   # (tools/site/media.py makes them)
        if not os.path.exists(os.path.join(OUT, 'media', f)):
            raise SystemExit(f'missing build/site/media/{f}')

    # downloads
    dl = os.path.join(OUT, 'downloads')
    os.makedirs(dl, exist_ok=True)
    mac_src = os.path.join(ROOT, 'build', 'release', 'VirtuaFighterRecomp-mac.zip')
    deck_src = os.path.join(ROOT, 'build', 'deck', 'VirtuaFighterRecomp-deck.tar.gz')
    win_src = os.path.join(ROOT, 'build', 'win', 'VirtuaFighterRecomp-windows.zip')
    for src in (mac_src, deck_src, win_src):
        if not carries(src, args.version):
            raise SystemExit(f'{os.path.relpath(src, ROOT)} is not the {args.version} build (rebuild it from the tag)')
    mac, deck, win = 'VirtuaFighterRecomp-mac.zip', 'VirtuaFighterRecomp-deck.tar.gz', 'VirtuaFighterRecomp-windows.zip'
    # The files themselves are on GitHub, attached to the release (mr-finster/vf1-recomp): the buttons link to the
    # latest release's, and the old /downloads/ links on this site send there too. The checks above still make sure
    # the packages built here, the ones to attach, are this version's.
    shutil.rmtree(dl)
    gh = 'https://github.com/mr-finster/vf1-recomp/releases/latest/download/'
    open(os.path.join(OUT, '_redirects'), 'w').write(''.join(f'/downloads/{f} {gh}{f} 302\n' for f in (mac, deck, win)))
    # a real 404 for any other path: without one Pages answers every unknown path with the page itself
    # (a 200), and a crawler following its relative links from a made-up path went ever deeper
    # (/img/shots/img/shots/...): VF3's site took 184,000 page loads in two hours that way (19 Oct)
    open(os.path.join(OUT, '404.html'), 'w').write('<!doctype html><meta charset="utf-8"><title>Not found</title>'
        '<p style="font:16px sans-serif;padding:2em">Not found. <a href="/">Virtua Fighter Recomp</a></p>\n')

    # the marquee (the app icon's letters, drawn wide) and the page icons
    img = os.path.join(OUT, 'img')
    os.makedirs(img, exist_ok=True)
    # the README's montage (game pictures: kept out of git, in build/readme, and served from the site)
    m = os.path.join(ROOT, 'build', 'readme', 'vf1_montage.jpg')
    if os.path.exists(m): shutil.copyfile(m, os.path.join(img, 'montage.jpg'))
    else: print('no build/readme/vf1_montage.jpg: no README montage on the site')
    # the changelog's lineup (the cast's and the secret characters' select portraits: game pictures, kept
    # out of git like the montage): build/readme/secret-characters.png, made small for the popup. Without it
    # the page leaves the picture out.
    lineup = os.path.join(ROOT, 'build', 'readme', 'secret-characters.png')
    if os.path.exists(lineup):
        im = Image.open(lineup).convert('RGB')
        im.resize((720, round(720 * im.height / im.width)), Image.LANCZOS).save(os.path.join(img, 'lineup.jpg'), quality=86, optimize=True)
    else: print('no build/readme/secret-characters.png: no lineup in the changelog')
    icon = Image.open(os.path.join(ROOT, 'assets', 'icon', 'icon.png')).convert('RGBA')
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'site', 'marquee.py'), os.path.join(img, 'marquee.png')], check=True)
    icon.resize((64, 64), Image.LANCZOS).save(os.path.join(img, 'icon-64.png'), optimize=True)
    icon.resize((180, 180), Image.LANCZOS).save(os.path.join(img, 'icon-180.png'), optimize=True)

    page = open(os.path.join(ROOT, 'tools', 'site', 'index.html')).read()
    today = datetime.date.today()
    subs = {
        '%SITE_URL%': args.url.rstrip('/'),
        '%VERSION%': args.version,
        '%DATE%': f'{today:%b} {today.day} {today.year}',
        '%MAC_URL%': gh + mac, '%DECK_URL%': gh + deck, '%WIN_URL%': gh + win,
        '%MAC_FILE%': mac, '%MAC_SIZE%': size(mac_src),
        '%DECK_FILE%': deck, '%DECK_SIZE%': size(deck_src),
        '%WIN_FILE%': win, '%WIN_SIZE%': size(win_src),
        '%SHOTS%': json.dumps([list(s) for s in SHOTS]),
    }
    for k, v in subs.items():
        page = page.replace(k, v)
    if not os.path.exists(lineup):   # (no picture: its figure left out)
        page = re.sub(r'\s*<figure class="lineup">.*?</figure>', '', page, flags=re.S)
    shutil.rmtree(os.path.join(OUT, 'functions'), ignore_errors=True)   # (an old build's, inside the site)
    os.makedirs(os.path.join(ROOT, 'functions'), exist_ok=True)
    ref_file = os.path.join(ROOT, '.refhit-url')
    ref = os.environ.get('VF_REF_URL') or (open(ref_file).read().strip() if os.path.exists(ref_file) else '')
    if ref: open(os.path.join(ROOT, 'functions', '_refhit.js'), 'w').write(REFHIT % json.dumps(ref + '?'))
    else: print('no .refhit-url (or VF_REF_URL): no referrer Function written')
    open(os.path.join(OUT, 'index.html'), 'w').write(page)
    # (every comment stripped from the page as served: tools/site/strip, its inline script's and styles' too)
    strip = os.path.join(ROOT, 'tools', 'site', 'strip')
    if not os.path.isdir(os.path.join(strip, 'node_modules')): subprocess.run(['npm', 'install', '--no-audit', '--no-fund'], cwd=strip, check=True)
    subprocess.run(['node', os.path.join(strip, 'strip.mjs'), os.path.join(OUT, 'index.html'), os.path.join(OUT, 'index.html')], check=True)
    shutil.copyfile(os.path.join(ROOT, 'tools', 'site', '_headers'), os.path.join(OUT, '_headers'))
    print('built', OUT)


if __name__ == '__main__':
    main()

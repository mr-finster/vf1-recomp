#!/usr/bin/env python3
"""The download page's video and pictures from the trailer (game pictures: they go only in build/site, never in git):
  build/site/media/screen.mp4   the cabinet's screen: the trailer's fights, 640x360, 30 fps, no sound track
  build/site/media/screen.webp  its first picture (the video's poster while it loads)
  build/site/media/poster.jpg   and media/og.jpg (the link preview)
The carousel's shots (build/site/shots) stay as they are. Needs ffmpeg and Pillow.

  tools/site/media.py TRAILER.mp4 [--from 9 --to 27.5 --still 19]
"""
import argparse, os, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'build', 'site')


def ff(*args):
    subprocess.run(['ffmpeg', '-v', 'error', '-y', *args], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trailer')
    ap.add_argument('--from', dest='t0', type=float, default=9)    # seconds into the online trailer: its fights,
    ap.add_argument('--to', dest='t1', type=float, default=27.5)   # before the randomized fighters' lineup
    ap.add_argument('--still', type=float, default=19)   # (Pai and Wolf by the lake)
    a = ap.parse_args()
    media = os.path.join(OUT, 'media')
    os.makedirs(media, exist_ok=True)
    ff('-ss', str(a.t0), '-to', str(a.t1), '-i', a.trailer, '-an', '-vf', 'scale=640:360:flags=lanczos,fps=30',
       '-c:v', 'libx264', '-preset', 'slow', '-crf', '24', '-pix_fmt', 'yuv420p', '-movflags', '+faststart',
       os.path.join(media, 'screen.mp4'))
    # (the WebP by Pillow, as build.py needs it anyway: not every ffmpeg has libwebp)
    png = os.path.join(media, 'screen.png')
    ff('-ss', str(a.t0), '-i', a.trailer, '-frames:v', '1', '-vf', 'scale=640:360:flags=lanczos', png)
    from PIL import Image
    Image.open(png).save(os.path.join(media, 'screen.webp'), quality=86)
    os.remove(png)
    for name in ('poster.jpg', 'og.jpg'):
        ff('-ss', str(a.still), '-i', a.trailer, '-frames:v', '1', '-vf', 'scale=1280:720:flags=lanczos',
           '-q:v', '3', os.path.join(media, name))
    print('build/site/media: screen.mp4, screen.webp, poster.jpg, og.jpg')


if __name__ == '__main__':
    main()

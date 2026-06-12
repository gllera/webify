#!/usr/bin/env python3
"""Render doc/img/graph-*.svg from a calibrate.sh results.csv.

    python3 doc/graphs.py --bench [/tmp/webify-calib]    # 1) time encodes
    python3 doc/graphs.py [/tmp/webify-calib/results.csv]  # 2) render

Stdlib only (the host has no plotting stack). Every chart plots the
*real-content* fixtures — vid5 (live action) for video/animations, the
photo pair for stills — because that is what the -q mappings are anchored
on; the synthetic fixtures' spread lives in the calibration docs' tables.
Comparing sizes at the same -q is fair by construction: every codec's -q
is an equal-SSIM fit, so -q buys the same look everywhere and size/time
are the only things left to compare. Colors are fixed mid-tones and the
background transparent so the SVGs read on both GitHub themes.

--bench times every codec x tier at the default -q through dist/webify
(WEBIFY= overrides) into DIR/times.csv. It runs strictly serially on
purpose — calibrate.sh's 16-way encode pool would inflate every wall
time with CPU contention — so run it on an idle box. Sizes come from the
harness cache; times come from here.
"""
import csv
import math
import os
import subprocess
import sys
import time

DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'img')


def bench(workdir):
    webify = os.environ.get('WEBIFY',
                            os.path.join(DIR, '..', 'dist', 'webify'))
    out = os.path.join(workdir, 'times.csv')
    # (codec tag, flags, fixture, output ext, repeats — fast encodes get
    # several runs and keep the best to shed scheduler noise)
    matrix = [('vp9', [], 'vid5.mp4', 'webm', 1),
              ('av1', ['--next'], 'vid5.mp4', 'webm', 1),
              ('x264', ['--legacy'], 'vid5.mp4', 'mp4', 1),
              ('webp', [], 'photo2d.png', 'webp', 3),
              ('avif', ['--next'], 'photo2d.png', 'avif', 3),
              ('animwebp', [], 'vid5.gif', 'webp', 2),
              ('animavif', ['--next'], 'vid5.gif', 'avif', 2)]
    rows = []
    for tag, flags, src, ext, n in matrix:
        fix = src.split('.')[0]
        for suf, tflag in (('', []), ('f', ['--fast']), ('b', ['--best'])):
            tmp = os.path.join(workdir, f'bench.tmp.{ext}')
            best = min(timed(webify, flags + tflag,
                             os.path.join(workdir, src), tmp)
                       for _ in range(n))
            os.unlink(tmp)
            rows.append((fix, tag + suf, f'{best:.3f}'))
            print(f'{fix:8} {tag + suf:10} {best:8.2f}s')
    csv.writer(open(out, 'w')).writerows(rows)
    print(f'wrote {out}')


def timed(webify, flags, src, dst):
    t = time.perf_counter()
    subprocess.run([webify, *flags, src, dst], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return time.perf_counter() - t


if len(sys.argv) > 1 and sys.argv[1] == '--bench':
    bench(sys.argv[2] if len(sys.argv) > 2 else '/tmp/webify-calib')
    sys.exit(0)

SRC = sys.argv[1] if len(sys.argv) > 1 else '/tmp/webify-calib/results.csv'

GRAY, BLUE, ORANGE, GREEN = '#8b949e', '#2f81f7', '#e8793a', '#3fb950'
W, H = 720, 420
ML, MR, MT, MB = 86, 16, 44, 52          # plot margins
FONT = 'font-family="ui-sans-serif,system-ui,sans-serif"'


def load(path):
    rows = {}                            # (fixture, codec, qtag) -> KB
    for name, size, _ in csv.reader(open(path)):
        fix, codec, qtag = name.rsplit('.', 1)[0].split('_', 2)
        rows[(fix, codec, qtag)] = int(size) / 1024
    return rows


def fmt_kb(v):
    return f'{v / 1024:g}M' if v >= 1024 else f'{v:g}K'


def svg_open(title):
    return [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
            f'width="{W}" height="{H}" {FONT} font-size="13">',
            f'<text x="{W / 2}" y="22" text-anchor="middle" fill="{GRAY}" '
            f'font-size="15" font-weight="600">{title}</text>']


def axis_text(x, y, s, anchor='middle', size=12):
    return (f'<text x="{x}" y="{y}" text-anchor="{anchor}" fill="{GRAY}" '
            f'font-size="{size}">{s}</text>')


def line_chart(out, title, series, xlabel, log2y=False):
    """series: [(label, color, [(x, kb)])]; y log2 gets power-of-2 ticks."""
    xs = sorted({x for _, _, pts in series for x, _ in pts})
    ys = [y for _, _, pts in series for _, y in pts]
    x0, x1 = min(xs), max(xs)
    if log2y:
        lo = 2 ** math.floor(math.log2(min(ys)))
        hi = 2 ** math.ceil(math.log2(max(ys)))
        yticks = [lo * 2 ** i for i in range(int(math.log2(hi / lo)) + 1)]
        ymap = lambda v: math.log2(v)
    else:
        hi = max(ys) * 1.15
        step = next(s for s in (2, 5, 10, 20, 50, 100, 200) if hi / s <= 8)
        yticks = [i * step for i in range(int(hi / step) + 2)]
        hi, lo = yticks[-1], 0
        ymap = lambda v: v
    py = lambda v: MT + (H - MT - MB) * (ymap(yticks[-1]) - ymap(max(v, 1e-9))) \
        / (ymap(yticks[-1]) - ymap(yticks[0]) or 1)
    px = lambda v: ML + (W - ML - MR) * (v - x0) / (x1 - x0)

    s = svg_open(title)
    for t in yticks:
        s.append(f'<line x1="{ML}" y1="{py(t):.1f}" x2="{W - MR}" '
                 f'y2="{py(t):.1f}" stroke="{GRAY}" stroke-opacity="0.25"/>')
        s.append(axis_text(ML - 8, py(t) + 4, fmt_kb(t), 'end'))
    for t in xs:
        s.append(axis_text(px(t), H - MB + 20, f'{t:g}'))
    s.append(axis_text((ML + W - MR) / 2, H - 12, xlabel))
    lx = ML + 10
    for label, color, pts in series:
        d = ' '.join(f'{px(x):.1f},{py(y):.1f}' for x, y in sorted(pts))
        s.append(f'<polyline points="{d}" fill="none" stroke="{color}" '
                 f'stroke-width="2.5"/>')
        for x, y in pts:
            s.append(f'<circle cx="{px(x):.1f}" cy="{py(y):.1f}" r="3.5" '
                     f'fill="{color}"/>')
        s.append(f'<rect x="{lx}" y="{MT - 12}" width="12" height="12" '
                 f'fill="{color}" rx="2"/>')
        s.append(axis_text(lx + 18, MT - 2, label, 'start'))
        lx += 36 + 7.2 * len(label)
    s.append('</svg>')
    open(out, 'w').write('\n'.join(s))
    print(f'wrote {out}')


def bar_chart(out, title, groups, tiers, tickfmt=fmt_kb,
              vfmt=lambda v: f'{v:.0f}'):
    """groups: [(name, {tier: value})]; one color per tier, labels on top."""
    vals = [v for _, d in groups for v in d.values()]
    hi = max(vals) * 1.2
    step = next(s for e in (-2, -1, 0, 1, 2) for b in (1, 2, 5)
                if hi / (s := b * 10 ** e) <= 8)
    yticks = [round(i * step, 2) for i in range(int(hi / step) + 2)]
    py = lambda v: MT + (H - MT - MB) * (1 - v / yticks[-1])

    s = svg_open(title)
    for t in yticks:
        s.append(f'<line x1="{ML}" y1="{py(t):.1f}" x2="{W - MR}" '
                 f'y2="{py(t):.1f}" stroke="{GRAY}" stroke-opacity="0.25"/>')
        s.append(axis_text(ML - 8, py(t) + 4, tickfmt(t), 'end'))
    gw = (W - ML - MR) / len(groups)
    bw = min(34, gw / (len(tiers) + 1.5))
    for gi, (name, d) in enumerate(groups):
        cx = ML + gw * (gi + 0.5)
        s.append(axis_text(cx, H - MB + 20, name, size=13))
        for ti, (tier, color) in enumerate(tiers):
            if tier not in d:
                continue
            x = cx + (ti - (len(tiers) - 1) / 2) * (bw + 4) - bw / 2
            v = d[tier]
            s.append(f'<rect x="{x:.1f}" y="{py(v):.1f}" width="{bw}" '
                     f'height="{py(0) - py(v):.1f}" fill="{color}" rx="2"/>')
            s.append(axis_text(x + bw / 2, py(v) - 5, vfmt(v), size=11))
    for ti, (tier, color) in enumerate(tiers):
        lx = ML + 10 + ti * 110
        s.append(f'<rect x="{lx}" y="{MT - 12}" width="12" height="12" '
                 f'fill="{color}" rx="2"/>')
        s.append(axis_text(lx + 18, MT - 2, tier, 'start'))
    s.append('</svg>')
    open(out, 'w').write('\n'.join(s))
    print(f'wrote {out}')


rows = load(SRC)
QV = [1, 3, 4.8, 7, 9]                   # video -q grid (def is its own crf)
qt = lambda q: 'def' if q == 'def' else f'{q:g}'


def pts(fix, codec, qs):
    out = [(8.0 if q == 'def' else q, rows[(fix, codec, qt(q))])
           for q in qs if (fix, codec, qt(q)) in rows]
    if not out:
        sys.exit(f'missing {fix}_{codec} in {SRC} — run calibrate.sh first')
    return out


def photo_mean(codec, q):
    k = [(f, codec, qt(q)) for f in ('photo1d', 'photo2d')]
    return sum(rows[x] for x in k) / 2 if all(x in rows for x in k) else None


# video: same look per -q, who pays how many bytes (default tier)
line_chart(os.path.join(DIR, 'graph-video-q.svg'),
           'Video, live action (vid5): size at the same look, default tier',
           [('H.264 (--legacy)', ORANGE, pts('vid5', 'x264', QV)),
            ('VP9 (default)', GRAY, pts('vid5', 'vp9', QV)),
            ('AV1 (--next)', BLUE, pts('vid5', 'av1', QV))],
           '-q  (equal visual quality per point)', log2y=True)

# video: what each effort tier costs in bytes at the default -q
bar_chart(os.path.join(DIR, 'graph-video-tiers.svg'),
          'Video, live action (vid5): size by effort tier, default -q (KB)',
          [('H.264 (--legacy)', {t: rows[('vid5', 'x264' + s, 'def')]
                    for t, s in (('default', ''), ('--fast', 'f'), ('--best', 'b'))}),
           ('VP9', {t: rows[('vid5', 'vp9' + s, 'def')]
                    for t, s in (('default', ''), ('--fast', 'f'), ('--best', 'b'))}),
           ('AV1 (--next)', {t: rows[('vid5', 'av1' + s, 'def')]
                    for t, s in (('default', ''), ('--fast', 'f'), ('--best', 'b'))})],
          [('--fast', ORANGE), ('default', BLUE), ('--best', GREEN)])

# stills: photo pair mean across the upper -q range (default tier)
sq = ['def', 8.5, 9, 9.5, 10]
line_chart(os.path.join(DIR, 'graph-stills-q.svg'),
           'Stills, photos (Kodak pair mean): size at the same look, default tier',
           [('WebP (default)', GRAY,
             [(8.0 if q == 'def' else q, photo_mean('webp', q)) for q in sq]),
            ('AVIF (--next)', BLUE,
             [(8.0 if q == 'def' else q, photo_mean('avif', q)) for q in sq])],
           '-q  (equal visual quality per point)')

# stills: tier cost at the default -q (the fast tiers pay for their speed
# differently: cwebp -m 4 pays bytes, AVIF pays the -4 CRF offset)
bar_chart(os.path.join(DIR, 'graph-stills-tiers.svg'),
          'Stills, photos (Kodak pair mean): size by effort tier, default -q (KB)',
          [('WebP', {t: photo_mean('webp' + s, 'def')
                     for t, s in (('default', ''), ('--fast', 'f'), ('--best', 'b'))}),
           ('AVIF (--next)', {t: photo_mean('avif' + s, 'def')
                     for t, s in (('default', ''), ('--fast', 'f'), ('--best', 'b'))})],
          [('--fast', ORANGE), ('default', BLUE), ('--best', GREEN)])

# animations: the headline gap, on the GIF class the curve is anchored to
line_chart(os.path.join(DIR, 'graph-anim-q.svg'),
           'Animated GIF, live action (vid5.gif): size at the same look, default tier',
           [('animated WebP (default)', GRAY, pts('vid5', 'animwebp', [5, 6.5, 'def', 9.5])),
            ('animated AVIF (--next)', BLUE, pts('vid5', 'animavif', [5, 6.5, 'def', 9.5]))],
           '-q  (equal visual quality per point)', log2y=True)

# encode time by tier, when a serial `--bench` run left times behind
tcsv = os.path.join(os.path.dirname(SRC), 'times.csv')
if os.path.exists(tcsv):
    t = {(f, c): float(s) for f, c, s in csv.reader(open(tcsv))}
    tierd = lambda fix, tag: {n: t[(fix, tag + s)] for n, s in
                              (('default', ''), ('--fast', 'f'),
                               ('--best', 'b')) if (fix, tag + s) in t}
    fmt_s = lambda v: f'{v:g}s'
    for out, title, groups in (
        ('graph-video-time.svg',
         'Video, live action (vid5, 6 s): encode time by tier, default -q',
         [('H.264 (--legacy)', tierd('vid5', 'x264')), ('VP9', tierd('vid5', 'vp9')),
          ('AV1 (--next)', tierd('vid5', 'av1'))]),
        ('graph-stills-time.svg',
         'Stills, photo (256x384): encode time by tier, default -q',
         [('WebP', tierd('photo2d', 'webp')),
          ('AVIF (--next)', tierd('photo2d', 'avif'))]),
        ('graph-anim-time.svg',
         'Animated GIF, live action (vid5.gif, 3 s): encode time by tier, default -q',
         [('animated WebP', tierd('vid5', 'animwebp')),
          ('animated AVIF (--next)', tierd('vid5', 'animavif'))])):
        bar_chart(os.path.join(DIR, out), title, groups,
                  [('--fast', ORANGE), ('default', BLUE), ('--best', GREEN)],
                  tickfmt=fmt_s, vfmt=lambda v: f'{v:.2g}s')
else:
    print(f'no {tcsv}: skipping the encode-time charts '
          f'(run `python3 doc/graphs.py --bench` first)')

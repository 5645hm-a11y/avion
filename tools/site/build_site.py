"""Builds the AVION 0.1 site, whitepaper and launch cards from templates and the measured head-to-head
(build/bench/summary.json, made by tools/bench/report.py). `{{ expression }}` in a template is evaluated with the summary
in scope (LG, CR: the two files' scan figures; FL, SV, PL: flow, survey, players); a figure that is missing stops the build.
  python tools/site/build_site.py --docs       docs/benchmarks.md only (it goes into the bundle, before the bundle's hash exists)
  python tools/site/build_site.py [artifact-dir]
writes docs/site/{index,whitepaper}.html (fonts from assets/fonts) with the bundle in docs/site/downloads, the posts and the
Hugging Face card, artifacts/marketing/avion-0.1-x-card{,-dark}.html and showcase-plates.html (tools/site/render_cards.py renders
them) and, with artifact-dir, copies for the Artifact viewer (no document wrapper, fonts from Google Fonts). The showcase's
figures come from build/showcase/showcase.json (tools/showcase/make_showcase.py)."""
import json, pathlib, re, shutil, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).parent
S = json.loads((ROOT / "build" / "bench" / "summary.json").read_text(encoding="utf-8"))
DOCS_ONLY = "--docs" in sys.argv
MANIFEST = {} if DOCS_ONLY else json.loads((ROOT / "artifacts" / "share" / "AVION-0.1-delivery-manifest.json").read_text(encoding="utf-8"))

LOCAL_FONTS = """<style>
@font-face { font-family: "Newsreader"; font-style: normal; font-weight: 200 800; font-display: swap; src: url("assets/fonts/Newsreader-normal.woff2") format("woff2"); }
@font-face { font-family: "Newsreader"; font-style: italic; font-weight: 200 800; font-display: swap; src: url("assets/fonts/Newsreader-italic.woff2") format("woff2"); }
@font-face { font-family: "Inter"; font-style: normal; font-weight: 100 900; font-display: swap; src: url("assets/fonts/InterVariable.woff2") format("woff2"); }
@font-face { font-family: "Inter"; font-style: italic; font-weight: 100 900; font-display: swap; src: url("assets/fonts/InterVariable-Italic.woff2") format("woff2"); }
@font-face { font-family: "JetBrains Mono"; font-style: normal; font-weight: 100 800; font-display: swap; src: url("assets/fonts/JetBrainsMono-normal.woff2") format("woff2"); }
</style>"""
GOOGLE_FONTS = ('<link rel="preconnect" href="https://fonts.googleapis.com"><link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>'
                '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Inter:ital,wght@0,300..800;1,300..800&family=JetBrains+Mono:wght@400;500;600;700'
                '&family=Newsreader:ital,opsz,wght@0,6..72,300..700;1,6..72,300..700&display=swap">')

# The woven mark, theme-aware: legs in the text colour, the wing in clay; each instance needs its own mask ids.
_count = [0]


def mark(size=26, label=False):
    _count[0] += 1
    u = f"m{_count[0]}"
    L, R, C = "104,440 152,440 280,76 232,76", "360,440 408,440 280,76 232,76", "40,342 70,304 488,282 470,300"
    def m(name, over):
        return (f'<mask id="{u}{name}" maskUnits="userSpaceOnUse" x="0" y="0" width="512" height="512"><rect width="512" height="512" fill="#fff"/>'
                f'<polygon points="{over}" fill="#000" stroke="#000" stroke-width="16"/></mask>')
    aria = 'role="img" aria-label="AVION"' if label else 'aria-hidden="true"'
    return (f'<svg viewBox="0 0 512 512" width="{size}" height="{size}" {aria}><defs>{m("l", R)}{m("c", L)}{m("r", C)}</defs>'
            f'<polygon points="{L}" fill="currentColor" mask="url(#{u}l)"/><polygon points="{R}" fill="currentColor" mask="url(#{u}r)"/>'
            f'<polygon points="{C}" fill="var(--clay)" mask="url(#{u}c)"/></svg>')


def n(x, d=2):
    return f"{x:,.{d}f}"


def frames(xs):
    """A list of picture numbers as prose: "765, 1,325 and 2,125"; "none" when empty."""
    xs = [f"{x:,}" for x in xs]
    return "none" if not xs else xs[0] if len(xs) == 1 else ", ".join(xs[:-1]) + " and " + xs[-1]


def suite(c):
    """A suite's result from its log: "210/210 passed" and, when any, which failed and which were skipped."""
    s = S["suites"][c]
    text = f"{s['passed']}/{s['total']} passed"
    if s.get("failed"): text += "; failed: " + ", ".join(s["failed"])
    if s.get("skipped"): text += "; skipped: " + ", ".join(s["skipped"])
    return text


def clean(c):
    return not S["suites"][c].get("failed") and not S["suites"][c].get("skipped")


def times(a, b, d=0):
    """How many times larger b is than a, as "1,234×"."""
    return f"{b / a:,.{d}f}×"


SC = json.loads((ROOT / "build" / "showcase" / "showcase.json").read_text(encoding="utf-8"))
SITE_URL = "https://5645hm-a11y.github.io/avion/"
REPO_URL = "https://github.com/5645hm-a11y/avion"  # The source (AGPLv3 or commercial, LICENSING.md).
_PRIVATE = ROOT / ".release-private.json"  # Local only: extra head markup for the hosted copy of the page.
VIEWER_META = json.loads(_PRIVATE.read_text(encoding="utf-8")).get("artifact_head", "") if _PRIVATE.exists() else ""
# Overlays on the engine's snapshots: fixed colours (they sit on pictures, not on the page), the 1280 x 720 image's coordinates.
AMBER, HOT, WHITE = "#FFC857", "#FF7A4D", "#FAF8F5"


def label(x, y, text, color=WHITE, size=21, anchor="start"):
    """A label on a picture: mono text on a dark plate sized to it (0.6 em a character)."""
    w = 0.66 * size * len(text) + 16  # Sans capitals; the pages measure the text and fit the plate (fitLabels).
    x0 = x - w if anchor == "end" else x
    return (f'<rect x="{x0:.1f}" y="{y - size - 4:.1f}" width="{w:.1f}" height="{size + 13:.1f}" rx="3" fill="#0E0E0D" fill-opacity=".72"/>'
            f'<text x="{x0 + 8:.1f}" y="{y + 2:.1f}" font-size="{size}" fill="{color}">{text}</text>')


def ov_perceive():
    P = SC["perceive"]
    x0, y0, x1, y1 = P["line"]
    parts = [f'<line x1="{x0:.1f}" y1="{y0:.1f}" x2="{x1:.1f}" y2="{y1:.1f}" stroke="{AMBER}" stroke-width="3" stroke-dasharray="14 9"/>',
             label(24, y0 - 22, f"HORIZON {P['horizon_deg']:+.2f}°  ·  APPLIED {P['tilt_deg']:+.2f}°", AMBER)]
    for o in P["objects"]:
        x, y, w, h = o["box"]
        c = HOT if o["approaching"] else WHITE
        parts.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="4" fill="none" stroke="{c}" stroke-width="3"/>')
        text = f"#{o['id']}  TTC {o['ttc']:.2f} s  ·  TRUTH {P['ttc_truth']:.2f} s" if o["approaching"] else f"#{o['id']}  MOVING  {o['v'][0]:+.2f} W/S"
        parts.append(label(x, y + h + 40, text, c) if o["approaching"] else label(x, y - 14, text, c))
    hx, hy = P["hazard"]
    parts.append(f'<circle cx="{hx:.1f}" cy="{hy:.1f}" r="16" fill="none" stroke="{HOT}" stroke-width="3"/>'
                 f'<path d="M{hx - 30:.1f} {hy:.1f}h18M{hx + 12:.1f} {hy:.1f}h18M{hx:.1f} {hy - 30:.1f}v18M{hx:.1f} {hy + 12:.1f}v18" stroke="{HOT}" stroke-width="3"/>')
    return f'<svg class="ov" viewBox="0 0 {P["size"][0]} {P["size"][1]}" aria-hidden="true">{"".join(parts)}</svg>'


def ov_pulse():
    P = SC["pulse"]
    x, y, w, h = P["region"]
    return (f'<svg class="ov" viewBox="0 0 {P["size"][0]} {P["size"][1]}" aria-hidden="true">'
            f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" fill="none" stroke="{WHITE}" stroke-width="3" stroke-dasharray="12 8"/>'
            + label(x, y - 14, f"VIBROMETRY  {P['bpm']:.2f} BPM  ·  TRUTH {P['bpm_truth']:.2f}", WHITE) + '</svg>')


def footer():
    """The pages' footer: the brand, two link columns, the credits and a base line (both pages link through index.html)."""
    return (f'<footer class="site-foot gutter">\n  <div class="wide foot-grid">\n'
            f'    <div><a class="brand" href="index.html#top">{mark(26)} AVION <small>0.1</small></a>'
            '<p>A deterministic perception, vision and media kernel behind one C99 header, for machines that have to see.</p></div>\n'
            '    <nav class="foot-col" aria-label="Product"><h4>Product</h4><a href="index.html#capabilities">Capabilities</a><a href="index.html#showcase">Showcase</a>'
            '<a href="index.html#benchmarks">Benchmarks</a><a href="whitepaper.html">Whitepaper</a></nav>\n'
            '    <nav class="foot-col" aria-label="Release"><h4>Release</h4><a href="index.html#download">Download</a><a href="downloads/AVION-0.1-delivery-manifest.json">Manifest</a>'
            '<a href="downloads/LICENSING.md">Licence</a><a href="index.html#method">Method</a><a href="https://github.com/5645hm-a11y/avion">GitHub</a></nav>\n'
            '    <div class="foot-col"><h4>Credits</h4><p>Typefaces: Inter, Newsreader and JetBrains Mono, under the SIL Open Font License. Showcase footage: '
            '<i>Big Buck Bunny</i> © 2008 Blender Foundation, <a style="display:inline;padding:0" href="https://creativecommons.org/licenses/by/3.0/">CC BY 3.0</a>; '
            'underexposed, shaken, tilted or pulsed for the tests as each plate states.</p></div>\n  </div>\n'
            f'  <div class="wide foot-base"><span>AVION 0.1 · SDK API 17 · released 2 November 2026, updated 10 November</span>'
            '<span>Every figure is generated from the measured runs: see the method.</span></div>\n</footer>')


def quickstart(raw=False):
    """The snippet between the markers of tools/showcase/quickstart.c (which builds and runs); HTML-escaped unless raw."""
    src = (ROOT / "tools" / "showcase" / "quickstart.c").read_text(encoding="utf-8")
    body = src.split("/* quickstart: begin */\n", 1)[1].split("/* quickstart: end */", 1)[0].rstrip()
    return body if raw else body.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


_demo = ROOT / "artifacts" / "marketing" / "avion-0.1-demo.mp4"
NS = {"DEMO_MB": f"{_demo.stat().st_size / 1e6:.1f}" if _demo.exists() else "0", "S": S, "SC": SC, "SITE_URL": SITE_URL, "REPO_URL": REPO_URL, "ov_perceive": ov_perceive, "ov_pulse": ov_pulse, "quickstart": quickstart, "footer": footer, "LG": S["scan"]["lg_black_4k60_hdr.mp4"], "CR": S["scan"]["costa_rica_4k60_hdr.mp4"], "FL": S["flow"], "SV": S["survey"],
      "PL": S["players"], "M": MANIFEST, "n": n, "times": times, "mark": mark, "round": round, "min": min, "max": max, "abs": abs, "len": len, "frames": frames, "P0": S.get("previous", {}), "SU": S["suites"], "suite": suite, "clean": clean}


def render(name, fonts):
    text = (HERE / name).read_text(encoding="utf-8")
    text = text.replace("{{CSS}}", (HERE / "site.css").read_text(encoding="utf-8")).replace("{{FONTS}}", fonts)
    text = text.replace("{{DATA}}", json.dumps(S, separators=(",", ":"))).replace("{{SHOW}}", json.dumps(SC, separators=(",", ":")))
    return re.sub(r"\{\{(.+?)\}\}", lambda m: str(eval(m.group(1).strip(), {"__builtins__": {}, **NS})), text, flags=re.S)


def strip_document(html):
    """The Artifact viewer wraps the page in its own document: keep only what goes inside it."""
    html = re.sub(r"^\s*<!doctype html>\s*<html[^>]*>\s*<head>\s*<meta charset=\"utf-8\">\s*<meta name=\"viewport\"[^>]*>\s*", "", html, flags=re.I)
    return html.replace("</head>\n<body>\n", "", 1).replace("</body>\n</html>\n", "")


(ROOT / "docs" / "benchmarks.md").write_text(render("benchmarks.md", ""), encoding="utf-8", newline="\n")
if DOCS_ONLY:
    sys.exit(0)
out = ROOT / "docs" / "site"
for page in ("index.html", "whitepaper.html"):
    (out / page).write_text(render(page, LOCAL_FONTS), encoding="utf-8")
for post in ("X_POST.md", "LINKEDIN_POST.md", "HUGGINGFACE_README.md", "HUGGINGFACE_POST.md", "HACKER_NEWS_SHOW_HN.md", "REDDIT_POSTS.md"):
    (ROOT / "artifacts" / "marketing" / post).write_text(render(post, ""), encoding="utf-8", newline="\n")
# The plates (showcase pictures with their overlays, for the model card and the posts): tools/site/render_cards.py cuts them.
plates = render("plates.html", LOCAL_FONTS).replace("<head>\n", '<head>\n<base href="../../docs/site/">\n', 1)
(ROOT / "artifacts" / "marketing" / "showcase-plates.html").write_text(plates, encoding="utf-8")
# The download beside the pages (GitHub Pages serves docs/site as it is).
(out / "downloads").mkdir(exist_ok=True)
for name in ("AVION-0.1-Evaluation.zip", "AVION-0.1-delivery-manifest.json"):
    shutil.copyfile(ROOT / "artifacts" / "share" / name, out / "downloads" / name)
# API 14: the packages and the licence beside the bundle.
for name in ("avion-0.1.0-py3-none-win_amd64.whl", "Avion.Sdk.0.1.0.nupkg"):
    shutil.copyfile(ROOT / "artifacts" / "packages" / name, out / "downloads" / name)
shutil.copyfile(ROOT / "LICENSING.md", out / "downloads" / "LICENSING.md")
shutil.copyfile(ROOT / "LICENSING.md", out / "downloads" / "LICENSE.md")  # Links published before 2026-11-10.
shutil.copyfile(ROOT / "LICENSE", out / "downloads" / "LICENSE.txt")  # The AGPLv3 text (Option A).
# The launch reel (tools/showcase/make_demo_video.py) beside the pictures: the hero plays it.
for name in ("avion-0.1-demo.mp4", "avion-0.1-demo.webm", "avion-0.1-demo-poster.jpg"):
    shutil.copyfile(ROOT / "artifacts" / "marketing" / name, out / "assets" / name)
for theme in ("light", "dark"):
    card = render("card.html", LOCAL_FONTS.replace("assets/fonts/", "../../docs/site/assets/fonts/"))
    # Pinned either way: the renderer follows the system's theme otherwise.
    card = card.replace('<html lang="en">', f'<html lang="en" data-theme="{theme}">', 1)
    (ROOT / "artifacts" / "marketing" / f"avion-0.1-x-card{'-dark' if theme == 'dark' else ''}.html").write_text(card, encoding="utf-8")
if len(sys.argv) > 1:
    art = pathlib.Path(sys.argv[1]); art.mkdir(parents=True, exist_ok=True)
    # The Artifact copy downloads from the published site.
    viewer = lambda h: h.replace("</title>\n", "</title>\n" + VIEWER_META, 1)  # The Artifact viewer's hint, in its copy only.
    (art / "avion-site.html").write_text(viewer(strip_document(render("index.html", GOOGLE_FONTS))).replace('href="downloads/', f'href="{SITE_URL}downloads/').replace('href="index.html#', 'href="#').replace('"assets/avion-0.1-demo', f'"{SITE_URL}assets/avion-0.1-demo'), encoding="utf-8")
    (art / "whitepaper.html").write_text(viewer(render("whitepaper.html", GOOGLE_FONTS)), encoding="utf-8")
print("built", ", ".join(p.name for p in out.glob("*.html")))

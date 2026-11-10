"""The launch hero (2026-11-10): AVION's wordmark, a one-line claim and four measured figures over a key visual, 3200 x 1800.
  python -I tools/site/launch_hero.py [background.png]

Writes artifacts/marketing/avion-launch-hero.png (over the background, default artifacts/marketing/avion-openart-hero.png) and
avion-launch-overlay.png (the same type and logo on a transparent background, for compositing over another render). The figures
are those of build/bench/summary.json and the SDK's host-decode test; the logo is docs/site/assets/avion-01-logo-dark.svg as it is.
"""
import json, pathlib, subprocess, sys, tempfile
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
MK = ROOT / "artifacts" / "marketing"
ASSETS = ROOT / "docs" / "site" / "assets"
CHROME = pathlib.Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe")
S = json.loads((ROOT / "build" / "bench" / "summary.json").read_text(encoding="utf-8"))
SCAN = S["scan"]["lg_black_4k60_hdr.mp4"]["avion_linux_us"]
FLOW = S["flow"]["avion_linux_ms"]
background = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else MK / "avion-openart-hero.png"

FIGURES = [("0", "", "heap allocations per 1,000 frames", "ffmpeg: 132,292"),
           (f"{SCAN:.2f}", "µs", "to scan a 4K HEVC frame for cuts", "from slice headers, no decode"),
           (f"{FLOW:.2f}", "ms", "optical flow, 640×480, one core", "8.5× OpenCV DIS ultrafast"),
           ("0.7", "%", "of one core, H.264 decoded on the GPU", "into shared D3D11/D3D12 textures")]


def page(with_background):
    fonts = (ASSETS / "fonts").as_uri()
    logo = (ASSETS / "avion-01-logo-dark.svg").as_uri()
    figures = "".join(f'<div class="f"><b>{v}<small>{u}</small></b><span>{a}</span><i>{b}</i></div>' for v, u, a, b in FIGURES)
    bg = (f'<img class="bg" src="{background.as_uri()}" alt=""><div class="shade"></div>') if with_background else ""
    return f"""<!doctype html><html><head><meta charset="utf-8"><style>
@font-face {{ font-family: Inter; src: url("{fonts}/InterVariable.woff2") format("woff2"); font-weight: 100 900; }}
@font-face {{ font-family: Newsreader; src: url("{fonts}/Newsreader-normal.woff2") format("woff2"); font-weight: 200 800; }}
html, body {{ margin: 0; width: 1600px; height: 900px; overflow: hidden; background: {"#111110" if with_background else "transparent"}; }}
body {{ position: relative; font-family: Inter, sans-serif; color: #F5F4F0; font-feature-settings: "tnum" 1, "lnum" 1, "zero" 0; }}
.bg {{ position: absolute; inset: 0; width: 100%; height: 100%; object-fit: cover; }}
.shade {{ position: absolute; inset: 0; background:
  linear-gradient(90deg, rgba(17,17,16,.88) 0%, rgba(17,17,16,.62) 30%, rgba(17,17,16,0) 58%),
  linear-gradient(0deg, rgba(17,17,16,.92) 0%, rgba(17,17,16,.55) 22%, rgba(17,17,16,0) 42%); }}
.logo {{ position: absolute; left: 70px; top: 56px; height: 112px; }}
.claim {{ position: absolute; left: 80px; top: 222px; width: 540px; }}
.claim h1 {{ margin: 0; font: 400 66px/1.04 Newsreader, serif; letter-spacing: -.01em; }}
.claim h1 em {{ font-style: normal; color: #D97757; }}
.claim p {{ margin: 22px 0 0; font: 400 20px/1.45 Inter, sans-serif; color: #CFCBC2; }}
.figs {{ position: absolute; left: 80px; right: 80px; bottom: 92px; display: grid; grid-template-columns: repeat(4, 1fr); border-top: 1px solid rgba(245,244,240,.38); }}
.f {{ padding: 20px 28px 0 0; }}
.f b {{ display: block; font: 650 56px/1 Inter, sans-serif; letter-spacing: -.035em; color: #F5F4F0; }}
.f b small {{ font: 500 24px/1 Inter; margin-left: 6px; color: #D97757; letter-spacing: 0; }}
.f span {{ display: block; margin-top: 12px; font: 500 17px/1.3 Inter; color: #E9E5DC; }}
.f i {{ display: block; margin-top: 4px; font: 400 15px/1.3 Inter; font-style: normal; color: #A8A498; }}
.foot {{ position: absolute; left: 80px; right: 80px; bottom: 40px; display: flex; justify-content: space-between; font: 500 15px/1 Inter; color: #A8A498; letter-spacing: .01em; }}
.foot b {{ color: #F5F4F0; font-weight: 600; }}
</style></head><body>{bg}
<img class="logo" src="{logo}" alt="AVION 0.1">
<div class="claim"><h1>Sees the world.<br><em>Allocates nothing.</em></h1>
<p>A perception and media engine for drones, robots and AI: stabilised, levelled, tracked and decoded on the GPU, behind one C99 header.</p></div>
<div class="figs">{figures}</div>
<div class="foot"><span><b>Open source</b> · AGPLv3 or commercial · C++20 · Direct3D 12</span><span>github.com/5645hm-a11y/avion</span></div>
</body></html>"""


def shoot(html_text, png, transparent):
    with tempfile.TemporaryDirectory() as tmp:
        html = pathlib.Path(tmp) / "hero.html"
        html.write_text(html_text, encoding="utf-8")
        args = [str(CHROME), "--headless=new", "--disable-gpu", "--hide-scrollbars", f"--user-data-dir={tmp}/profile", "--allow-file-access-from-files",
                "--force-device-scale-factor=2", "--window-size=1600,900", "--virtual-time-budget=5000", f"--screenshot={png}", html.as_uri()]
        if transparent:
            args.insert(-1, "--default-background-color=00000000")
        subprocess.run(args, check=True, capture_output=True)
    im = Image.open(png)
    assert im.size == (3200, 1800), im.size
    print(png.name, im.size, im.mode)


if __name__ == "__main__":
    if not background.exists():
        sys.exit(f"no background: {background}")
    shoot(page(True), MK / "avion-launch-hero.png", False)
    shoot(page(False), MK / "avion-launch-overlay.png", True)

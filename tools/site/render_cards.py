"""Renders the launch cards and showcase plates with headless Chrome, and checks the posts' lengths (after build_site.py).
  python tools/site/render_cards.py
- artifacts/marketing/avion-0.1-x.png, avion-0.1-x-dark.png: the 1600x900 card at device scale 2 (3200x1800);
- docs/site/assets/showcase/plate-{lift,perceive,events,pulse}.png (and copies in artifacts/marketing/): the plates, 1600x900;
- X_POST.md: every post at most 280 characters with each URL counted as 23; LINKEDIN_POST.md at most 3,000. Fails otherwise."""
import pathlib, re, shutil, subprocess, sys, tempfile
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
MK = ROOT / "artifacts" / "marketing"
SHOW = ROOT / "docs" / "site" / "assets" / "showcase"
CHROME = pathlib.Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe")


def shoot(html, png, width, height, scale):
    with tempfile.TemporaryDirectory() as profile:  # Its own profile: the user's Chrome may be open.
        subprocess.run([str(CHROME), "--headless=new", "--disable-gpu", "--hide-scrollbars", f"--user-data-dir={profile}", "--allow-file-access-from-files",
                        f"--force-device-scale-factor={scale}", f"--window-size={width},{height}", "--virtual-time-budget=4000",
                        f"--screenshot={png}", html.as_uri()], check=True, capture_output=True)
    im = Image.open(png)
    assert im.size == (width * scale, height * scale), (png, im.size)


for theme in ("", "-dark"):
    shoot(MK / f"avion-0.1-x-card{theme}.html", MK / f"avion-0.1-x{theme}.png", 1600, 900, 2)
    print("card", f"avion-0.1-x{theme}.png")
# The link preview (og:image): the light card at 1200 x 675, what X and LinkedIn show for a link to the site.
Image.open(MK / "avion-0.1-x.png").convert("RGB").resize((1200, 675), Image.LANCZOS).save(ROOT / "docs" / "site" / "assets" / "og-card.png", optimize=True)
print("link preview og-card.png")
sheet = MK / "showcase-plates.png"
shoot(MK / "showcase-plates.html", sheet, 1600, 3600, 1)
im = Image.open(sheet).convert("RGB")
for i, name in enumerate(("lift", "perceive", "events", "pulse")):
    plate = im.crop((0, 900 * i, 1600, 900 * (i + 1)))
    plate.save(SHOW / f"plate-{name}.png", optimize=True)
    shutil.copyfile(SHOW / f"plate-{name}.png", MK / f"plate-{name}.png")
    print("plate", name)
sheet.unlink()

failed = False
for part in (MK / "X_POST.md").read_text(encoding="utf-8").split("\n---\n")[1:]:
    body = re.sub(r"^\s*\*\*\d+/\d+\*\*\s*\n", "", part.strip())
    n = len(re.sub(r"https?://\S+", "x" * 23, body))
    print(f"X post: {n} characters")
    failed |= n > 280
li = (MK / "LINKEDIN_POST.md").read_text(encoding="utf-8").split("\n---\n", 1)[1].strip()
print(f"LinkedIn: {len(li)} characters")
failed |= len(li) > 3000
sys.exit(1 if failed else 0)

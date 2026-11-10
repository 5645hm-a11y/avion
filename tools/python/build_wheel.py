"""Builds the AVION Python wheel (avion-<version>-py3-none-win_amd64.whl) with the standard library only.

    python tools/python/build_wheel.py <avion.dll> <out_dir> [--license LICENSING.md] [--header include/media/c_api.h]

A wheel is a zip archive (PEP 427): the package (tools/python/avion: __init__.py, _capi.py, plus avion.dll and c_api.h copied in),
and avion-<version>.dist-info with METADATA (core metadata 2.1), WHEEL, the licence and RECORD (each file's SHA-256, urlsafe
base64 without padding, and size). Reproducible: fixed timestamps, sorted entries. Prints the wheel's path."""
import base64
import hashlib
import pathlib
import re
import sys
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
TAG = "py3-none-win_amd64"
STAMP = (2026, 11, 4, 0, 0, 0)


def digest(data):
    return "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode("ascii")


def main(argv):
    if len(argv) < 3:
        print(__doc__); return 2
    dll, out = pathlib.Path(argv[1]), pathlib.Path(argv[2])
    options = dict(zip(argv[3::2], argv[4::2]))
    root = HERE.parents[1]
    licence = pathlib.Path(options.get("--license", root / "LICENSING.md"))
    header = pathlib.Path(options.get("--header", root / "include" / "media" / "c_api.h"))
    source = (HERE / "avion" / "__init__.py").read_text(encoding="utf-8")
    version = re.search(r'^__version__ = "([^"]+)"', source, re.M).group(1)
    readme = (HERE / "README.md").read_text(encoding="utf-8") if (HERE / "README.md").exists() else ""
    info = f"avion-{version}.dist-info"
    files = {
        "avion/__init__.py": source.encode("utf-8"),
        "avion/_capi.py": (HERE / "avion" / "_capi.py").read_bytes(),
        "avion/avion.dll": dll.read_bytes(),
        "avion/c_api.h": header.read_bytes(),
        f"{info}/LICENSING.md": licence.read_bytes(),
        f"{info}/LICENSE": (root / "LICENSE").read_bytes(),  # The AGPLv3 text (LICENSING.md, option A).
        f"{info}/METADATA": ("Metadata-Version: 2.1\nName: avion\nVersion: " + version + "\n"
                             "Summary: AVION media engine SDK for Python: GPU playback, perception telemetry, zero-copy tensors, event camera, bitstream perception\n"
                             "Author: Alexandre Heymann\nLicense: AGPL-3.0-only OR LicenseRef-AVION-Commercial\nLicense-File: LICENSE\nLicense-File: LICENSING.md\n"
                             "Home-page: https://5645hm-a11y.github.io/avion/\nProject-URL: Source, https://github.com/5645hm-a11y/avion\nRequires-Python: >=3.8\n"
                             "Classifier: License :: OSI Approved :: GNU Affero General Public License v3\nClassifier: License :: Other/Proprietary License\nClassifier: Operating System :: Microsoft :: Windows\n"
                             "Classifier: Programming Language :: Python :: 3\nClassifier: Topic :: Multimedia :: Video\n"
                             "Description-Content-Type: text/markdown\n\n" + readme).encode("utf-8"),
        f"{info}/WHEEL": f"Wheel-Version: 1.0\nGenerator: avion build_wheel.py\nRoot-Is-Purelib: false\nTag: {TAG}\n".encode("utf-8"),
    }
    record = "".join(f"{name},{digest(data)},{len(data)}\n" for name, data in sorted(files.items())) + f"{info}/RECORD,,\n"
    files[f"{info}/RECORD"] = record.encode("utf-8")
    out.mkdir(parents=True, exist_ok=True)
    wheel = out / f"avion-{version}-{TAG}.whl"
    with zipfile.ZipFile(wheel, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(files):
            entry = zipfile.ZipInfo(name, STAMP); entry.compress_type = zipfile.ZIP_DEFLATED; entry.external_attr = 0o644 << 16
            z.writestr(entry, files[name])
    print(wheel)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

"""package.py: the Steam Link app as build/chupathingyce.tgz, which the box
installs at boot from steamlink/apps/ on a USB stick, and as
build/chupathingyce-steamlink-release.zip, the same files, side by side as the
updater takes them (updater.c), for release.py to sign. The game alone, with
no map, sound or art of Halo's: the player's disc image gives those."""
import gzip
import io
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = Path(__file__).resolve().parent / "app"
BUILT = ROOT / "chupathingyce" / "build" / "steamlink" / "halo"
OUTPUT = ROOT / "build" / "chupathingyce.tgz"
UPDATE = ROOT / "build" / "chupathingyce-steamlink-release.zip"
STRIPPED = ROOT / "build" / "halo-stripped"
GAME = ROOT / "chupathingyce"
# the box's own certificate authorities are its firmware's, of 2015: Mozilla's,
# from https://curl.se/ca/cacert.pem, kept with the app
CERTIFICATES = APP / "ca-certificates.crt"
LICENSES = {
    "chupathingyce-LICENSE.txt": "chupathingyce/LICENSE",
    "SDL3-LICENSE.txt": "build/SDL/LICENSE.txt",
    "mbedtls-LICENSE.txt": "chupathingyce/port/third_party/mbedtls/LICENSE",
    "miniupnpc-LICENSE.txt": "chupathingyce/port/third_party/miniupnpc/LICENSE",
    "expat-COPYING.txt": "chupathingyce/port/third_party/expat/COPYING",
    "kcp-LICENSE.txt": "chupathingyce/port/third_party/kcp/LICENSE",
    "monocypher-LICENCE.md": "chupathingyce/port/third_party/monocypher/LICENCE.md",
    "qrcodegen-LICENSE.txt": "chupathingyce/port/third_party/qrcodegen/LICENSE",
    "stb-LICENSE.txt": "chupathingyce/port/third_party/stb/LICENSE",
    "tomlc17-LICENSE.txt": "chupathingyce/port/third_party/tomlc17/LICENSE",
    "zlib-LICENSE.txt": "chupathingyce/port/third_party/zlib/LICENSE",
    "Overpass-OFL.txt": "chupathingyce/port/assets/fonts/Overpass-OFL.txt",
    "CyborgInterfaceGothic-OFL.txt": "chupathingyce/port/assets/fonts/CyborgInterfaceGothic-OFL.txt",
    "OpenCE-OFL.txt": "chupathingyce/port/assets/fonts/OpenCE-OFL.txt",
    "NotoSans-OFL.txt": "chupathingyce/port/linux/ui/fonts/OFL.txt",
    "Kenney-Input-Prompts-CC0.txt": "chupathingyce/port/linux/ui/fonts/KENNEY-CC0.txt",
}


def add(tar, name, data, mode):
    info = tarfile.TarInfo(f"chupathingyce/{name}")
    info.size = len(data)
    info.mode = mode
    tar.addfile(info, io.BytesIO(data))


def main():
    subprocess.run(["llvm-strip", "-o", str(STRIPPED), str(BUILT)], check=True)
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w") as tar:
        add(tar, "toc.txt", (APP / "toc.txt").read_bytes(), 0o644)
        add(tar, "icon.png", (APP / "icon.png").read_bytes(), 0o644)
        add(tar, "run.sh", (APP / "run.sh").read_bytes(), 0o755)
        add(tar, "halo", STRIPPED.read_bytes(), 0o755)
        add(tar, "ca-certificates.crt", Path(CERTIFICATES).read_bytes(), 0o644)
        add(tar, "brokers.txt", (GAME / "port/assets/network/brokers.txt").read_bytes(), 0o644)
        # what the game and its libraries' licenses ask every copy to carry
        for name, source in LICENSES.items():
            add(tar, f"licenses/{name}", (ROOT / source).read_bytes(), 0o644)
    OUTPUT.write_bytes(gzip.compress(buffer.getvalue()))
    print(f"{OUTPUT}: {OUTPUT.stat().st_size >> 20} MB")
    # (the updater takes only files at a zip's top, and sets a program's mode
    # itself: run.sh and halo)
    with tarfile.open(OUTPUT) as tar, zipfile.ZipFile(UPDATE, "w", zipfile.ZIP_DEFLATED) as update:
        for member in tar.getmembers():
            update.writestr(member.name.split("/")[-1], tar.extractfile(member).read())
    print(f"{UPDATE}: {UPDATE.stat().st_size >> 20} MB")


if __name__ == "__main__":
    sys.exit(main())

"""publish.py [host]: build/release/ (release.py's) on the update server, where
the boxes' updaters look (port/linux/src/updater.c's UPDATE_API):

    https://warman.world/fastdl/chupathingyce-steamlink/
        releases/latest                     the version boxes should be on
        releases/download/v<version>/...    that release's files

Its files go up first and "latest" last, in one rename, so that a box never
hears of a version that is not all there. The signing key does not go up: a
server that is broken into still cannot make a box take an update.
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "release"
FOLDER = "/var/www/html/fastdl/chupathingyce-steamlink"
FILES = ["chupathingyce.tgz", "chupathingyce-steamlink-release.zip", "chupathingyce-steamlink-release.zip.sig",
         "updates.json", "updates.json.sig"]


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.2.253"
    version = json.loads((OUT / "updates.json").read_text(encoding="utf-8"))["version"]
    tag = f"v{version}"
    release = f"{FOLDER}/releases/download/{tag}"
    subprocess.run(["ssh", host, f"mkdir -p '{release}' && chmod -R a+rX,go-w '{FOLDER}'"], check=True)
    subprocess.run(["scp", "-q", *[str(OUT / name) for name in FILES], f"{host}:{release}/"], check=True)
    with tempfile.TemporaryDirectory() as directory:
        latest = Path(directory) / "latest"
        latest.write_text(json.dumps({"tag_name": tag}) + "\n", encoding="utf-8")
        subprocess.run(["scp", "-q", str(latest), f"{host}:{FOLDER}/releases/latest.new"], check=True)
    subprocess.run(["ssh", host, f"chmod -R a+rX,go-w '{FOLDER}' && mv -f '{FOLDER}/releases/latest.new' '{FOLDER}/releases/latest'"],
                   check=True)
    print(f"{tag} is what the boxes will be offered")


if __name__ == "__main__":
    main()

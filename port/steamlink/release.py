"""release.py <key.pem> [fix]: a Steam Link release's files in build/release/,
ready for a GitHub release tagged v<version>. The version is the game's, from
the submodule's VERSION (0.9.1), so a box says which game it is. A fix of the
port's own on the same game would need a version of its own: up to 0.9.0d the
game's updater took any text after the numbers in order (0.9.0d1 after
0.9.0d), but from 0.9.1 it reads only <n>.<n>.<n><letters>, with -testing or
-nightly after (update_manifest.c), and a box on such a build is never
offered a version of another shape, nor a fix number one in order (0.9.1 and
1 would be 0.9.11, past 0.9.2). So a fix number is refused here until the
updater takes one, and a release is only ever the game's version, so the boxes go through them in order:

    chupathingyce.tgz                           the first install, from a stick
    chupathingyce-steamlink-release.zip         what the game's updater takes
    chupathingyce-steamlink-release.zip.sig     its signature
    updates.json, updates.json.sig              the updater's list, signed

The game is built as a release of that version (only those look for updates),
and takes updates only when they are signed with the key whose public half it
is built with (port/linux/src/update_key.h). Keep the key out of every
repository, and keep a copy: without it no update reaches the boxes in the
field, whose players would have to install a new key's build by hand.
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAME = ROOT / "chupathingyce"
BUILD = ROOT / "build"
OUT = BUILD / "release"
ZIP = "chupathingyce-steamlink-release.zip"


def run(*command, **kwargs):
    subprocess.run([str(part) for part in command], check=True, **kwargs)


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and not sys.argv[2].isdigit()):
        sys.exit(__doc__)
    key = Path(sys.argv[1])
    version = (GAME / "VERSION").read_text(encoding="utf-8").strip() + (sys.argv[2] if len(sys.argv) == 3 else "")
    # (the shape the game's updater reads, update_manifest.c's version_parse:
    # anything else is never offered to a box, nor can a box on it be offered)
    if len(sys.argv) == 3:
        sys.exit("a fix number makes no version the game's updater (0.9.1 on) reads in order (0.9.1 and 1 is "
                 "0.9.11, past 0.9.2): see this file's top")
    if not re.fullmatch(r"\d{1,9}\.\d{1,9}\.\d{1,9}[a-z]*", version):
        sys.exit(f"{version} is no version the game's updater reads (update_manifest.c): no box would take it")
    if not key.is_file():
        sys.exit(f"no key at {key}")
    environment = dict(os.environ, HALO_VERSION=version, HALO_RELEASE_BUILD="1")
    run(sys.executable, "configure.py", "--release", "--console-port", "../port/steamlink", cwd=GAME, env=environment)
    run("ninja", "steamlink", cwd=GAME, env=environment)
    run(sys.executable, ROOT / "port" / "steamlink" / "package.py")
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    shutil.copy2(BUILD / "chupathingyce.tgz", OUT)
    shutil.copy2(BUILD / ZIP, OUT)
    sign = GAME / "tools" / "update_sign.py"
    run(sys.executable, sign, "sign", key, version, OUT / ZIP)
    run(sys.executable, sign, "manifest", key, version, "--tag", f"v{version}", OUT / ZIP)
    print(f"version {version}: tag it v{version}")
    for path in sorted(OUT.iterdir()):
        print(f"{path.name}: {path.stat().st_size} bytes")


if __name__ == "__main__":
    main()

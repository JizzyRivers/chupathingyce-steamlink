"""The Steam Link port's build (configure.py --console-port: chupathingyce's
tools/console_build.py), ``ninja steamlink``.

The game and the platform layer are the 32-bit Linux build's
(chupathingyce's tools/linux_build.py, through Linux32Units as the 32-bit
dedicated server builds them), compiled for the 2015 Steam Link: 32-bit ARM
(armv7-a, NEON, hard float) against valve's steamlink-sdk, whose root
filesystem is the box's (glibc 2.19 and its libraries) and whose toolchain
has gcc 9.2's crt files and libgcc. The compiler is clang: any clang that
has ARM, so the one on Linux, or LLVM's on Windows. The game's few x86
assumptions in the shared code are patches/chupathingyce.

The result is build/steamlink/halo, linked with SDL3 (static, built with
port/steamlink/toolchain.cmake: the workflow builds it).

Run from the game's folder, which the 32-bit build's rules are written for:

    cd chupathingyce
    python3 configure.py --release --console-port ../port/steamlink
    ninja steamlink

--steamlink-sdk is valve's sdk (MARVELL_SDK_PATH, or steamlink-sdk in or
beside this repository), --steamlink-sdl SDL3 installed for the box
(build/sdl3 in this repository).
"""

import os
from pathlib import Path
from types import SimpleNamespace
from typing import Any, List, Optional

from tools.embed_assets import hud_assets_build, hud_configure_inputs, ui_fonts_build
from tools.console_build import console_defines
from tools.linux_build import Linux32Units, _load_port_config, compile_launcher, linux32_objects

# the box's processor: a 1 GHz ARMv7 core with NEON (VFPv3, no fused
# multiply-add), hard float as the sdk's glibc is
STEAMLINK_TRIPLE = "armv7a-linux-gnueabihf"
STEAMLINK_INSTRUCTION_SET = ["-march=armv7-a", "-mfpu=neon", "-mfloat-abi=hard"]
# the game's char is MSVC's, signed (ARM's is unsigned): as the arm64 server
STEAMLINK_ABI_FLAGS = ["-fsigned-char", "-DHALO_STEAMLINK"]
# the box's 256 MB (about 200 free): the Xbox's texture cache (22 MB, not the
# desktop's 256) and its structure surfaces and dynamic triangles a frame,
# as the Xbox 360's port has them (port/linux/include/halo_port_capacity.h)
STEAMLINK_CAPABILITIES = ["-DHALO_CONSOLE_XBOX_RENDER_SIZES",
                          # the renderer's ES path on the box's ES 2, which
                          # src/gles2_layer.c fills out to the ES 3 it uses
                          "-DHALO_CONSOLE_GLES2"]
# the port's own platform units
STEAMLINK_SOURCES = ["src/gles2_layer.c"]
# the sdk's gcc, for its crt files and libgcc (clang finds glibc in the root
# filesystem)
GCC_VERSION_DIR = Path("toolchain/lib/gcc/armv7a-cros-linux-gnueabi/9.2.0")
# SDL3's own (its sdl3.pc's Libs): EGL is the box's vivante driver's
STEAMLINK_LIBRARIES = ["SDL3", "EGL", "m", "pthread", "dl", "rt"]


def add_arguments(parser: Any) -> None:
    parser.add_argument("--steamlink-sdk", type=Path, default=None,
                        help="valve's steamlink-sdk (default: MARVELL_SDK_PATH, or steamlink-sdk in or beside "
                        "this repository)")
    parser.add_argument("--steamlink-sdl", type=Path, default=None,
                        help="SDL3 built and installed for the box (default: build/sdl3 in this repository)")
    parser.add_argument("--steamlink-cc", default="clang", help="the compiler: a clang that has ARM")


def configure_inputs(port: Any) -> List[Path]:
    return [port.path("toolchain.cmake"), port.path("src"), *hud_configure_inputs()]


def _repository(port: Any) -> Path:
    """this repository's root: port/steamlink's grandparent"""
    return port.root.resolve().parent.parent


def _sdk(sln: Any, port: Any) -> Optional[Path]:
    given = getattr(sln.args, "steamlink_sdk", None) or os.environ.get("MARVELL_SDK_PATH")
    candidates = [Path(given)] if given else [_repository(port) / "steamlink-sdk",
                                               _repository(port).parent / "steamlink-sdk"]
    for sdk in candidates:
        if (sdk / "rootfs").is_dir() and (sdk / GCC_VERSION_DIR).is_dir():
            return sdk.resolve()
    return None


def _posix(path: Path) -> str:
    """a path for the compiler's command line (forward slashes on Windows too)"""
    return path.as_posix()


def generate(n: Any, sln: Any, port: Any) -> None:
    sdk = _sdk(sln, port)
    sdl = (getattr(sln.args, "steamlink_sdl", None) or _repository(port) / "build" / "sdl3")
    if sdk is None or not (Path(sdl) / "include" / "SDL3" / "SDL.h").is_file():
        n.comment("Steam Link (ninja steamlink): not generated, no steamlink-sdk or no SDL3 built for it "
                  "(port/steamlink/build.py)")
        return
    sdl = Path(sdl).resolve()
    config = _load_port_config()
    build_dir: Path = sln.build_dir / "steamlink"
    output = build_dir / "halo"
    cc = getattr(sln.args, "steamlink_cc", "clang")
    target = [f"--target={STEAMLINK_TRIPLE}", f"--sysroot={_posix(sdk / 'rootfs')}",
              f"--gcc-install-dir={_posix(sdk / GCC_VERSION_DIR)}"]
    # (the 32-bit Linux build's MSVC semantics headers, ``ninja linux``'s
    # rules make them)
    linux_dir = sln.build_dir / "linux"

    n.comment("Steam Link (ninja steamlink; port/steamlink/build.py)")
    n.variable("steamlink_cc", cc)
    n.rule(
        name="steamlink_cc",
        command=f"{compile_launcher(sln)}$steamlink_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="STEAMLINK CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="steamlink_link",
        # (on Windows the response file's paths have backslashes, which
        # clang's default quoting takes for escapes)
        command="$steamlink_cc $ldflags -o $out " + ("--rsp-quoting=windows " if os.name == "nt" else "")
        + "@$out.rsp $libs",
        description="STEAMLINK LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )

    # the game list's overlay and its fonts come with the game browser, which
    # the box leaves out (no keyboard or mouse to use it with)
    steamlink_sln = SimpleNamespace(**vars(sln))
    steamlink_sln.game_browser = False
    generated = hud_assets_build(n, "steamlink", build_dir / "generated" / "hud_hires_assets.c")
    generated += ui_fonts_build(n, "steamlink", build_dir / "generated" / "ui_fonts.c", steamlink_sln)
    units = Linux32Units(
        sln=steamlink_sln, config=config, game_browser=False,
        semantics_header=linux_dir / "halo_msvc_semantics.h",
        platform_semantics_header=linux_dir / "platform_msvc_semantics.h",
        embedded_assets=generated, rule="steamlink_cc", target_flags=target,
        instruction_set=STEAMLINK_INSTRUCTION_SET,
        # (and configure.py's console options: --release, --console-players)
        extra_flags=[*STEAMLINK_ABI_FLAGS, *STEAMLINK_CAPABILITIES, *console_defines(sln)],
        platform_sources=[port.path(source) for source in STEAMLINK_SOURCES],
        # (ES 3's headers, Khronos', for its names: the sdk has ES 2's)
        include_flags=[f"-I{_posix(port.path('include'))}", f"-I{_posix(sdl / 'include')}"],
    )
    objects = linux32_objects(n, units, build_dir / "obj", [], [])
    n.build(
        outputs=output,
        rule="steamlink_link",
        inputs=objects,
        variables={
            "ldflags": " ".join([*target, *STEAMLINK_INSTRUCTION_SET, "-fuse-ld=lld", "-no-pie", "-g",
                                 f"-L{_posix(sdl / 'lib')}"]),
            "libs": " ".join(f"-l{library}" for library in STEAMLINK_LIBRARIES),
        },
        implicit=[sdl / "lib" / "libSDL3.a"],
    )
    n.build(outputs="steamlink", rule="phony", inputs=output)
    n.newline()

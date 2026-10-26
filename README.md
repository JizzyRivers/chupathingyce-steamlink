chupathingyce-steamlink

halo ce on the og steam link box (the 2015 one). native, not streamed. port of chupathingyce https://github.com/ChupathingyCE/chupathingyce

nothing u can see yet. the game boots on the box and sits in the main menu w no picture (no gles2 renderer yet), so its a very expensive radio w no sound either rn

bring ur own copy, same as every other chupathingyce build

the box
- 1 core armv7 (32 bit, hard float, neon) at 1ghz, about 256mb ram
- opengl es 2.0 only
- linux 3.8, glibc 2.19, valve's sdk ships sdl2 and an old sdl3 (3.1.5)

why 32 bit is good news
the game wants 32 bit pointers. android has to fake that (arm64 w 32 bit pointers, lots of tricks). the steam link just is 32 bit, so this is closer to the 32 bit linux build cross compiled for arm

the plan
1. probe: does sdl3 open a gles2 window and see the controller on the box. done, it does: vivante gc1000, 1280x720 at 60, 360 pad works (probe/)
2. armhf linux build of the game, straight on the box (no pi needed). done: it boots, loads ui.map, builds the menus and runs the main loop at about 115mb. port/steamlink
3. a gles2 renderer path, main menu first. the game's gl path wants desktop gl or gles 3.2, so rn sdl says no window and the game runs blind
4. sound: the box has no alsa card 0, valve's audio goes some other way. to figure out
5. pack it as a steam link app

memory stuff so nobody panics
- the xbox's texture cache (22mb) not the desktop's 256mb, like the 360 port (HALO_CONSOLE_XBOX_RENDER_SIZES)
- the xbox window is at 0x80000000 like the 32 bit linux build, the box's 3g/1g split leaves room
- maps and saves go on /mnt/scratch (1.8gb), the save folder's z/ cache alone is about 750mb so the whole maps folder (1.7gb) doesnt fit next to it. u get a few maps for now

trying the probe
- grab the steamlink-probe artifact from the probe workflow in actions
- put the steamlink folder on a usb drive, plug it into the steam link, power cycle it
- run ChupathingyCE probe from the apps. start + back quits
- send me probe.log (it's next to the program, ssh in to grab it)

building the game
clang (any clang w arm, llvm's on windows works too) + valve's sdk (https://github.com/ValveSoftware/steamlink-sdk) for the box's glibc and gcc's crt files. the sdk's kernel folder has a file called aux.c which windows wont check out, so sparse checkout rootfs and toolchain/lib/gcc only
- clone the sdk next to this as steamlink-sdk (or set MARVELL_SDK_PATH)
- sdl3 3.4.16 w port/steamlink/toolchain.cmake, the options .github/workflows/probe.yml uses, installed to build/sdl3
- git submodule update --init, then apply patches/chupathingyce/*.patch to it (git -C chupathingyce apply ../patches/chupathingyce/0001-steamlink-port-hooks.patch)
- cd chupathingyce && python3 configure.py --release --console-port ../port/steamlink && ninja steamlink
- out comes chupathingyce/build/steamlink/halo. strip it, scp it to the box, ssh in and run it next to a maps folder

the patch is the game's x86 bits made arm friendly (fpscr for the control word, arm crash registers, glibc 2.19 gaps, misaligned float loads like the ppc ports). goes upstream eventually, capisce

building the probe
github actions builds it w valve's sdk so u just need linux if u do it urself: build sdl3 like .github/workflows/probe.yml does, then probe/build_steamlink.sh

chupathingyce is the game (submodule)

gpl 3 like the game. fan project, not microsoft / bungie / 343 / valve

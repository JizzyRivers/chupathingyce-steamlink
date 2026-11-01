chupathingyce-steamlink

halo ce on the og steam link box (the 2015 one). native, not streamed. port of chupathingyce https://github.com/ChupathingyCE/chupathingyce

campaign plays, the chupathingy menus r in, online games lists real servers n u can join them. its slow tho: about 21 fps on bloodgulch standing at the spawn w performance mode on, the ship in a10 is worse. goal is 25, working on it. this is a test build, expect stuff

bring ur own copy, same as every other chupathingyce build. theres no halo in this repo or the download, ur disc gives the game its maps

installing
u need
- an og steam link w ssh or not, doesnt matter
- a usb stick, fat32 is fine, 8gb or more (the disc image n what it makes come to about 7.5gb)
- ur halo: combat evolved xbox disc image, .iso or .xiso. a fat32 stick wont hold a file over 4gb so if urs is bigger trim it to an .xiso first
- a controller the box sees. wired 360 pad is what ive tested

steps
1. grab chupathingyce.tgz from the latest release
2. on the stick make steamlink/apps/ n put chupathingyce.tgz in it
3. put ur disc image on the root of the stick, not in a folder
4. optional, for custom edition servers: put halocesetup_en_1.00.exe on the root of the stick too
5. plug the stick in the steam link n power cycle it. it installs at boot
6. run ChupathingyCE from the apps
7. first run it copies ur maps to the stick, about 14 min, then custom edition if u added it, about 3 min, then makes its cache, about 3 min. a ring fills up n monks sing. only happens once, go eat something, capisce

leave the stick in. maps, saves, settings n the log all live on it in chupathingyce/, the box doesnt have the room. pull it while its running n ur on ur own

updates
when a new version is out the game asks at startup, A updates, B not now, Y dont ask again. updates come from warman.world n r signed so the box only takes ours. versions follow chupathingyce's, 0.8.1d is the game's 0.8.1d, a steam link only fix on top is 0.8.1d1

if it breaks
send me chupathingyce/run.log off the stick, thats the good stuff

the box
- 1 core armv7 (32 bit, hard float, neon) at 1ghz, about 256mb ram
- opengl es 2.0 only
- linux 3.8, glibc 2.19, valve's sdk ships sdl2 and an old sdl3 (3.1.5)

why 32 bit is good news
the game wants 32 bit pointers. android has to fake that (arm64 w 32 bit pointers, lots of tricks). the steam link just is 32 bit, so this is closer to the 32 bit linux build cross compiled for arm

the plan
1. probe: does sdl3 open a gles2 window and see the controller on the box. done, it does: vivante gc1000, 1280x720 at 60, 360 pad works (probe/)
2. armhf linux build of the game, straight on the box (no pi needed). done: it boots, loads ui.map, builds the menus and runs the main loop at about 115mb. port/steamlink
3. a gles2 renderer path, main menu first. menu done: the game's gles 3 path (the android one) on top of port/steamlink/src/gles2_layer.c, which fakes the gles 3 bits on the box's gles 2 (vaos, samplers, blits, sized formats, swizzles, glsl es 3.00 rewritten as 1.00). levels done too: a10 draws w its skinned characters. one skinned bump shader still doesnt fit the gc1000 so that kind of thing doesnt draw
4. framerate, in progress. where a frame goes rn: the vivante driver charges about 0.45 ms a draw no matter how small, and a10 does 200 to 400 draws. the gpu's pixel work is another 50 to 80 ms. fixed so far: sound mixing (was 37% of the cpu), the driver spamming stderr, a useless per pixel divide, and indices copied every draw

performance mode
on by default. leaves out the level's decals, dynamic lights, reflections, specular and the lens flares, draws at 320x240 scaled up (display.performance_lines), fogs out the distance outside cutscenes and clips there (display.performance_view_distance, 60 units) and drops models to coarser detail sooner (display.performance_model_detail), demake style. hold back + dpad down (or f9) to flip it in game, display.performance_mode in config.toml for how it starts. it also skips the sky (the fog color fills in), the see thru bits on the level's objects, plants n stuff u cant touch, and the hud's frames and backgrounds. run.sh turns off a vivante shader compiler thing (VC_ENABLE_LTC=0) that was redoing work every draw, that alone was about a sixth of the frame. bloodgulch's spawn is about 21fps rn (47 ms) w the game running, goal is 25, not there yet. merging draws can only reach about a quarter of them, most change a program or a texture, so its not the whole answer
5. sound: done, the box has no alsa card 0 so it goes thru the firmware's own sdl2 (port/steamlink/src/steamlink_audio.c)
6. pack it as a steam link app: done, port/steamlink/package.py, n port/steamlink/release.py builds n signs a release

vivante gc1000 gotchas, learned the hard way (each one hangs the gpu till the kernel resets it 20 sec later)
- texture2D w a lod bias on some draws. the layer samples w/o the bias (the game's biases are 0 anyway)
- vertex buffers: its glBufferSubData flushes the cpu cache over the whole buffer, the game streams into 16mb ones so a frame took seconds. the layer keeps buffers in plain memory and draws from client arrays
- 9 varyings is one too many (it has 8), 16 attributes too (it has 10). the layer drops the ones a shader never reads
- if u hang it hard enough the box needs a reboot -f, plain reboot doesnt cut it

memory stuff so nobody panics
- the xbox's texture cache (22mb) not the desktop's 256mb, like the 360 port (HALO_CONSOLE_XBOX_RENDER_SIZES)
- the xbox window is at 0x80000000 like the 32 bit linux build, the box's 3g/1g split leaves room
- the box's /mnt/scratch is 1.8gb, the maps r 1.8gb n the cache another 750mb, so it all goes on the stick (port/steamlink/app/run.sh mounts it, the firmware wont)

trying the probe
- grab the steamlink-probe artifact from the probe workflow in actions
- put the steamlink folder on a usb drive, plug it into the steam link, power cycle it
- run ChupathingyCE probe from the apps. start + back quits
- send me probe.log (it's next to the program, ssh in to grab it)

building the game
clang (any clang w arm, llvm's on windows works too) + valve's sdk (https://github.com/ValveSoftware/steamlink-sdk) for the box's glibc and gcc's crt files. the sdk's kernel folder has a file called aux.c which windows wont check out, so sparse checkout rootfs and toolchain/lib/gcc only
- clone the sdk next to this as steamlink-sdk (or set MARVELL_SDK_PATH)
- sdl3 3.4.16 w port/steamlink/toolchain.cmake, the options .github/workflows/probe.yml uses, installed to build/sdl3
- git submodule update --init (its on chupathingyce v0.8.1d), then apply patches/chupathingyce/*.patch to it (git -C chupathingyce apply ../patches/chupathingyce/0001-steamlink-port-hooks.patch)
- cd chupathingyce && python3 configure.py --release --console-port ../port/steamlink && ninja steamlink
- out comes chupathingyce/build/steamlink/halo. python port/steamlink/package.py makes build/chupathingyce.tgz to install from a stick
- a release needs its version from the submodule n the signing key: python port/steamlink/release.py <key.pem> [fix number]. the key stays off every repo, nobody touches the key

the patch is the game's x86 bits made arm friendly (fpscr for the control word, arm crash registers, glibc 2.19 gaps, misaligned float loads like the ppc ports), HALO_GL_ES for the renderer's gles path on a console w its own gl layer (HALO_CONSOLE_GLES2), and a fix for settings that only existed on the switch build. goes upstream eventually, capisce

seeing the picture w/o a tv: HALO_GLES2_SHOT=folder saves every 300th frame as a ppm. HALO_PERF_LOG=1 logs where each frame goes (world, objects, transparent, present...) w draws per part. HALO_PERF_AB freezes the game and flips script globals on n off to time them on the same picture. HALO_GLES2_MERGE_STATS=1 counts what each draw changes from the last one. HALO_GLES2_SLOW, _TRACE, _PROFILE and _EXPERIMENT are the gpu hang hunting tools (gles2_layer.c says what each does)

port/steamlink/include is khronos' gles 3 and gl2ext headers (mit), the sdk only has old gles 2 ones

building the probe
github actions builds it w valve's sdk so u just need linux if u do it urself: build sdl3 like .github/workflows/probe.yml does, then probe/build_steamlink.sh

chupathingyce is the game (submodule)

gpl 3 like the game. fan project, not microsoft / bungie / 343 / valve

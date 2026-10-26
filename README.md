chupathingyce-steamlink

halo ce on the og steam link box (the 2015 one). native, not streamed. port of chupathingyce https://github.com/ChupathingyCE/chupathingyce

nothing plays yet. this is day one

bring ur own copy, same as every other chupathingyce build

the box
- 1 core armv7 (32 bit, hard float, neon) at 1ghz, about 256mb ram
- opengl es 2.0 only
- linux 3.8, glibc 2.19, valve's sdk ships sdl2

why 32 bit is good news
the game wants 32 bit pointers. android has to fake that (arm64 w 32 bit pointers, lots of tricks). the steam link just is 32 bit, so this is closer to the 32 bit linux build cross compiled for arm

the plan
1. probe: does sdl3 open a gles2 window and see the controller on the box (probe/, in progress)
2. armhf linux build of the game, tested on a pi first
3. a gles2 renderer path, main menu first. the game's gles path is gles 3.2 now
4. pack it as a steam link app

trying the probe
- grab the steamlink-probe artifact from the probe workflow in actions
- put the steamlink folder on a usb drive, plug it into the steam link, power cycle it
- run ChupathingyCE probe from the apps. start + back quits
- send me probe.log (it's next to the program, ssh in to grab it)

building
github actions builds it w valve's sdk (https://github.com/ValveSoftware/steamlink-sdk) so u just need linux if u do it urself: clone the sdk next to this as steamlink-sdk, build sdl3 like .github/workflows/probe.yml does, then probe/build_steamlink.sh

chupathingyce is the game (submodule)

gpl 3 like the game. fan project, not microsoft / bungie / 343 / valve

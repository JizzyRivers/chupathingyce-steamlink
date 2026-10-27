#!/bin/sh
# the game as the steam link launches an app (shell.sh, with its interface
# closed): its folders on /mnt/scratch, its log in run.log
cd /mnt/scratch/halo
export HALO_SAVE_ROOT=/mnt/scratch/halo/save
[ -f launch.env ] && . ./launch.env
exec ./halo > run.log 2>&1

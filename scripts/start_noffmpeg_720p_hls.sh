#!/bin/sh
set -eu
URL=${1:-http://74.91.26.218:82/live/cctv1hd.m3u8}
PLAYER=${PLAYER:-/root/cedar_drm_player.yuvlive}
WIDTH=${WIDTH:-1280}
HEIGHT=${HEIGHT:-720}
FPS=${FPS:-25}
ROOT=/dev/shm/cctv-noffmpeg-$$
H264FIFO=$ROOT/video.h264
SEG=$ROOT/segment.ts
ES=$ROOT/segment.h264
mkdir -p "$ROOT"
mkfifo "$H264FIFO"
CPID= MPID=
cleanup() {
    trap - EXIT INT TERM
    [ -n "$MPID" ] && kill "$MPID" 2>/dev/null || true
    [ -n "$CPID" ] && kill "$CPID" 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT INT TERM
"$PLAYER" --raw-h264 "$WIDTH" "$HEIGHT" "$FPS" "$H264FIFO" >/tmp/cctv_noffmpeg_cedar.log 2>&1 &
CPID=$!
exec 3>"$H264FIFO"
last=
base=${URL%/*}/
while kill -0 "$CPID" 2>/dev/null; do
    line=$(wget -qO- "$URL" | grep '^cctv1md/' | tail -1 || true)
    [ -n "$line" ] || { sleep 2; continue; }
    [ "$line" = "$last" ] && { sleep 2; continue; }
    wget -qT 8 -t 1 -O "$SEG" "$base$line" || { rm -f "$SEG"; sleep 2; continue; }
    last=$line
    /usr/bin/mplayer -really-quiet -nosound -demuxer lavf -dumpvideo -dumpfile "$ES" "$SEG" >/tmp/cctv_noffmpeg_demux.log 2>&1 || true
    [ -s "$ES" ] || { rm -f "$SEG"; continue; }
    cat "$ES" >&3 || break
    rm -f "$ES"
    rm -f "$SEG"
done
wait "$CPID" 2>/dev/null || true

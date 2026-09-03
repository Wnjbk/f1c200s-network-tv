#!/bin/sh
# CCTV-1 candidate: HLS MPEG-TS -> native TS H.264 extractor -> Cedar FIFO.
# No FFmpeg, MPlayer, local media input, persistent media cache, or audio.
set -eu

URL=${1:-http://cd-live-stream.news.cctvplus.com/live/smil:CHANNEL2.smil/playlist.m3u8}
PLAYER=${PLAYER:-/root/cedar_drm_player.yuvlive}
EXTRACTOR=${EXTRACTOR:-$(dirname "$0")/ts_h264_extract}
WIDTH=${WIDTH:-640}
HEIGHT=${HEIGHT:-360}
FPS=${FPS:-25}
ROOT=/dev/shm/cctv1-640-native-ts-$$
MASTER=$ROOT/master.m3u8
PLAYLIST=$ROOT/playlist.m3u8
SEGMENT=$ROOT/segment.ts
FIFO=$ROOT/video.h264
PLAYER_LOG=${PLAYER_LOG:-/dev/shm/cctv1-640-native-ts-player.log}
NO_PACE=${CEDAR_NO_PACE:-1}

mkdir -p "$ROOT"
mkfifo "$FIFO"
PLAYER_PID=

cleanup() {
    trap - EXIT INT TERM
    [ -n "$PLAYER_PID" ] && kill "$PLAYER_PID" 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT INT TERM

[ -x "$PLAYER" ] || { echo "missing Cedar player: $PLAYER" >&2; exit 1; }
[ -x "$EXTRACTOR" ] || { echo "missing TS extractor: $EXTRACTOR" >&2; exit 1; }

if [ "$NO_PACE" = 1 ]; then
    CEDAR_NO_PACE=1 "$PLAYER" --raw-h264 "$WIDTH" "$HEIGHT" "$FPS" "$FIFO" >"$PLAYER_LOG" 2>&1 &
else
    "$PLAYER" --raw-h264 "$WIDTH" "$HEIGHT" "$FPS" "$FIFO" >"$PLAYER_LOG" 2>&1 &
fi
PLAYER_PID=$!
exec 3>"$FIFO"

last=
base=${URL%/*}/
while kill -0 "$PLAYER_PID" 2>/dev/null; do
    wget -qT 10 -t 1 -O "$MASTER" "$URL" || { sleep 2; continue; }
    # The selected CCTV source is a master playlist. Resolve only its
    # advertised 640x360 variant; direct media playlists still work too.
    variant=$(sed -n '/RESOLUTION=640x360/{n;p;q;}' "$MASTER")
    if [ -n "$variant" ]; then
        case "$variant" in
            http://*|https://*) variant_url=$variant ;;
            *) variant_url=$base$variant ;;
        esac
        wget -qT 10 -t 1 -O "$PLAYLIST" "$variant_url" || { sleep 2; continue; }
    else
        cp "$MASTER" "$PLAYLIST"
    fi
    line=$(sed -n '/^[^#]/p' "$PLAYLIST" | tail -n 1)
    [ -n "$line" ] || { sleep 2; continue; }
    [ "$line" = "$last" ] && { sleep 2; continue; }
    case "$line" in
        http://*|https://*) segment_url=$line ;;
        *) segment_url=$base$line ;;
    esac
    wget -qT 12 -t 1 -O "$SEGMENT" "$segment_url" || { rm -f "$SEGMENT"; sleep 2; continue; }
    last=$line
    "$EXTRACTOR" "$SEGMENT" >&3 || { rm -f "$SEGMENT"; break; }
    rm -f "$SEGMENT"
done

wait "$PLAYER_PID" 2>/dev/null || true

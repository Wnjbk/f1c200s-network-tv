# CCTV-1 640x360 Native TS Candidate

This candidate targets the 700 Kbit/s, 640x360 variant advertised by:

```text
http://cd-live-stream.news.cctvplus.com/live/smil:CHANNEL2.smil/playlist.m3u8
```

The public channel directory labels this source CCTV-1. Treat that label as
unverified until a board USB HDMI capture visibly shows CCTV-1 branding.

Pipeline:

```text
HLS playlist -> BusyBox wget TS segment in /dev/shm
             -> ts_h264_extract (PAT/PMT, type 0x1b only)
             -> Annex-B H.264 FIFO -> existing Cedar player -> DRM/HDMI
```

It does not invoke FFmpeg or MPlayer, does not use a local media input, and
does not write video segments to persistent storage. `ts_h264_extract` is
compiled as a Buildroot ARM user-space binary with:

```sh
/home/wnk/LicheePi_Nano/buildroot-2018.02.11/output/host/bin/arm-buildroot-linux-gnueabi-gcc \
  --sysroot=/home/wnk/LicheePi_Nano/buildroot-2018.02.11/output/host/arm-buildroot-linux-gnueabi/sysroot \
  -Os -Wall -Wextra -o ts_h264_extract tools/ts_h264_extract.c
```

The launcher keeps Cedar pacing enabled by default (`CEDAR_NO_PACE=0`): HLS
downloads complete a ten-second segment as a burst, while display pacing lets
the FIFO apply back-pressure instead of trying to decode that burst at storage
speed. Set `CEDAR_NO_PACE=1` only for a bounded diagnostic; it is not a live
playback setting.

Before extracting a segment, `ts_h264_extract` verifies complete 188-byte TS
packet sync through the end of the file. A malformed WLAN download emits no
H.264 bytes and the launcher discards it, then retries; it never turns a
partial segment into a false end of the Cedar FIFO stream.

The candidate must be deployed only to a new board directory. Acceptance
requires live source bytes, a running direct Cedar process, and two USB HDMI
captures showing different advancing CCTV-1 frames. Any other channel, local
video, FFmpeg/MPlayer process, missing picture, or unstable board state
rejects the candidate.

# CCTV-1 640x360 Native TS Candidate

This candidate targets the 700 Kbit/s, 640x360 variant advertised by:

```text
https://cd-live-stream.news.cctvplus.com/live/smil:CHANNEL2.smil/playlist.m3u8
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

The candidate must be deployed only to a new board directory. Acceptance
requires live source bytes, a running direct Cedar process, and two USB HDMI
captures showing different advancing CCTV-1 frames. Any other channel, local
video, FFmpeg/MPlayer process, missing picture, or unstable board state
rejects the candidate.

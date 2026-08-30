# Runtime Manifest

| Component | Board path | Required value |
| --- | --- | --- |
| Cedar player | `/root/cedar_drm_player.yuvlive` | MD5 `bbb7161eb694bd6f3898b86c7c3bf270` |
| FFmpeg | `/usr/bin/ffmpeg` | supports `h264_mp4toannexb` |
| WLAN | `wlan0` | associated with DHCP address and Internet access |
| Input | HLS | `http://74.91.26.218:82/live/cctv1hd.m3u8` |
| Video | H.264 | 1280x720 at 25 fps |
| FIFO | `/tmp/network_tv.h264` | Annex-B H.264 |

The low-memory startup mode (`TV_LOWMEM=1`, default) temporarily stops only
GMenu2X and the kernel/userspace logging daemons that were already running,
then drops reclaimable caches before starting playback. `stop` restores the
services that were stopped. It does not stop networking, Dropbear, the AIC
Miracast control process, load AIC modules, replace the kernel, or alter the
stable WLAN configuration. FFmpeg probe/demux buffering is bounded by the
`TV_FFMPEG_PROBESIZE` and `TV_FFMPEG_ANALYZEDURATION` variables.

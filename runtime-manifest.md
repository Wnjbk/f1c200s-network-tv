# Runtime Manifest

| Component | Board path | Required value |
| --- | --- | --- |
| Cedar player | `/root/cedar_drm_player.yuvlive` | MD5 `bbb7161eb694bd6f3898b86c7c3bf270` |
| FFmpeg | `/usr/bin/ffmpeg` | supports `h264_mp4toannexb` |
| WLAN | `wlan0` | associated with DHCP address and Internet access |
| Input | HLS | `http://74.91.26.218:82/live/cctv1hd.m3u8` |
| Video | H.264 | 1280x720 at 25 fps |
| FIFO | `/tmp/network_tv.h264` | Annex-B H.264 |

The startup script intentionally stops only its own player/FFmpeg process.
It does not load AIC modules, modify Miracast, replace the kernel, or alter
the stable control WLAN configuration.

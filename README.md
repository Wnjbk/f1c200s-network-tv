# F1C200S Network TV

Verified F1C200S network-TV runtime for SII9022 HDMI output:

```text
wlan0 -> FFmpeg HLS demux -> Annex-B H.264 FIFO -> Cedar hardware decode -> DRM -> HDMI
```

The default channel is CCTV-1 HD (`1280x720@25`) and uses:

```text
http://74.91.26.218:82/live/cctv1hd.m3u8
```

## Runtime

The target must already have the matching `cedar_drm_player.yuvlive`, Cedar
libraries/modules, FFmpeg and an associated `wlan0` control network. Start:

```sh
./scripts/start_network_tv.sh start
./scripts/start_network_tv.sh status
./scripts/start_network_tv.sh stop
```

Override source or geometry without editing the script:

```sh
TV_URL='http://example/live.m3u8' TV_WIDTH=1280 TV_HEIGHT=720 TV_FPS=25 \
  ./scripts/start_network_tv.sh restart
```

## Verified Evidence

On 2026-08-30 the board played CCTV-1 through the above pipeline. The tested
runtime player had MD5 `bbb7161eb694bd6f3898b86c7c3bf270`.

`player-src/` is the complete yuvlive player source snapshot. The runtime
binary and large third-party CedarX/FFmpeg dependencies are intentionally not
committed; their required target paths and hashes are listed in
`runtime-manifest.md`.

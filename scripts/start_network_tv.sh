#!/bin/sh
set -u

BASE=${TV_BASE:-/root/roms/tv/candidates/f1tv_https_mplayer_20260826}
IFACE=${TV_IFACE:-wlan0}
SSID=${TV_SSID:-wnk641_2.4G}
CHANNEL_URL=${TV_URL:-http://74.91.26.218:82/live/cctv1hd.m3u8}
WIDTH=${TV_WIDTH:-1280}
HEIGHT=${TV_HEIGHT:-720}
FPS=${TV_FPS:-25}
FIFO=${TV_FIFO:-/tmp/network_tv.h264}
LOG_DIR=${TV_LOG_DIR:-/root/roms/tv/logs}
STA_CONF=${TV_STA_CONF:-/etc/wpa_supplicant.conf}
SUPP=${TV_SUPP:-/root/aic_miracast/wpa24_aic_wfd_supplicant}
CLI=${TV_CLI:-/root/aic_miracast/wpa24_aic_wfd_cli}
PLAYER=${TV_PLAYER:-/root/cedar_drm_player.yuvlive}
FFMPEG=${TV_FFMPEG:-ffmpeg}
USB_BACKUP=${TV_USB_BACKUP:-/root/roms/kernel_backups/sii9022_cedar_physaddr_v571_deploy_20260824}
LOWMEM=${TV_LOWMEM:-1}
FFMPEG_PROBESIZE=${TV_FFMPEG_PROBESIZE:-32768}
FFMPEG_ANALYZEDURATION=${TV_FFMPEG_ANALYZEDURATION:-0}
PID_DIR=/tmp/network-tv-oneclick

log() { echo "[network-tv] $*"; }
die() { log "ERROR: $*"; exit 1; }
running() { [ -f "$1" ] && kill -0 "$(cat "$1" 2>/dev/null)" 2>/dev/null; }

pause_services() {
    [ "$LOWMEM" = 1 ] || return 0
    pidof gmenu2x >/dev/null 2>&1 && { : > "$PID_DIR/gmenu2x_was_running"; killall gmenu2x 2>/dev/null || true; }
    pidof syslogd >/dev/null 2>&1 && { : > "$PID_DIR/syslogd_was_running"; /etc/init.d/S01logging stop >/dev/null 2>&1 || killall syslogd 2>/dev/null || true; }
    pidof klogd >/dev/null 2>&1 && { : > "$PID_DIR/klogd_was_running"; killall klogd 2>/dev/null || true; }
    sync
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
}

restore_services() {
    [ "$LOWMEM" = 1 ] || return 0
    [ -f "$PID_DIR/gmenu2x_was_running" ] && /root/run_gmenu2x.sh >/dev/null 2>&1 &
    [ -f "$PID_DIR/syslogd_was_running" ] && /etc/init.d/S01logging start >/dev/null 2>&1 || true
    rm -f "$PID_DIR/gmenu2x_was_running" "$PID_DIR/syslogd_was_running" "$PID_DIR/klogd_was_running"
}

start_usb() {
    [ -d /sys/class/net/$IFACE ] && return 0
    [ -r "$USB_BACKUP/phy-generic.ko" ] || die "missing phy-generic.ko"
    [ -r "$USB_BACKUP/sunxi.ko" ] || die "missing sunxi.ko"
    insmod "$USB_BACKUP/phy-generic.ko" 2>/dev/null || true
    insmod "$USB_BACKUP/sunxi.ko" 2>/dev/null || true
    i=0
    while [ "$i" -lt 30 ] && [ ! -d /sys/class/net/$IFACE ]; do sleep 1; i=$((i + 1)); done
    [ -d /sys/class/net/$IFACE ] || die "$IFACE did not appear after USB host init"
}

start_wifi() {
    mkdir -p /var/run/wpa_supplicant "$LOG_DIR" "$PID_DIR"
    ip link set "$IFACE" up 2>/dev/null || ifconfig "$IFACE" up
    if ! $CLI -i "$IFACE" status 2>/dev/null | grep -q 'wpa_state=COMPLETED'; then
        killall wpa24_aic_wfd_supplicant 2>/dev/null || true
        rm -f "/var/run/wpa_supplicant/$IFACE" "/run/wpa_supplicant/$IFACE"
        "$SUPP" -B -D nl80211,wext -i "$IFACE" -c "$STA_CONF" \
            -f "$LOG_DIR/network_tv_wpa.log" || die "supplicant start failed"
        i=0
        while [ "$i" -lt 20 ]; do
            $CLI -i "$IFACE" status 2>/dev/null | grep -q 'wpa_state=COMPLETED' && break
            sleep 1; i=$((i + 1))
        done
    fi
    $CLI -i "$IFACE" status 2>/dev/null | grep -q 'wpa_state=COMPLETED' || die "Wi-Fi association failed"
    # Older Buildroot images do not ship iproute2; use either ip or BusyBox ifconfig.
    if ! $CLI -i "$IFACE" status 2>/dev/null | grep -q 'ip_address='; then
        udhcpc -i "$IFACE" -q -n -t 5 -T 2 >/dev/null 2>&1 || true
    fi
    if command -v ip >/dev/null 2>&1; then
        ip addr show "$IFACE" 2>/dev/null | grep -q 'inet ' || die "DHCP failed on $IFACE"
    else
        ifconfig "$IFACE" 2>/dev/null | grep -q 'inet addr:' || die "DHCP failed on $IFACE"
    fi
}

start() {
    running "$PID_DIR/player.pid" && die "TV already running; use status or stop"
    [ -x "$PLAYER" ] || die "missing Cedar player: $PLAYER"
    command -v "$FFMPEG" >/dev/null 2>&1 || die "missing ffmpeg"
    [ -x "$SUPP" ] || die "missing dedicated supplicant: $SUPP"
    start_usb
    start_wifi
    mkdir -p "$PID_DIR"
    pause_services
    rm -f "$FIFO"
    mkfifo "$FIFO" || die "cannot create FIFO"
    "$PLAYER" --raw-h264 "$WIDTH" "$HEIGHT" "$FPS" "$FIFO" \
        >"$LOG_DIR/network_tv_cedar.log" 2>&1 &
    echo $! > "$PID_DIR/player.pid"
    sleep 2
    setsid "$FFMPEG" -nostdin -y -loglevel error \
        -probesize "$FFMPEG_PROBESIZE" -analyzeduration "$FFMPEG_ANALYZEDURATION" \
        -fflags nobuffer -avioflags direct -max_delay 0 -i "$CHANNEL_URL" \
        -an -c:v copy -bsf:v h264_mp4toannexb -f h264 "$FIFO" \
        >"$LOG_DIR/network_tv_ffmpeg.log" 2>&1 &
    echo $! > "$PID_DIR/ffmpeg.pid"
    log "started $CHANNEL_URL on $IFACE ($WIDTHx$HEIGHT@$FPS)"
}

stop() {
    for f in "$PID_DIR/ffmpeg.pid" "$PID_DIR/player.pid"; do
        if [ -f "$f" ]; then kill "$(cat "$f" 2>/dev/null)" 2>/dev/null || true; rm -f "$f"; fi
    done
    killall ffmpeg 2>/dev/null || true
    rm -f "$FIFO"
    restore_services
    log "stopped"
}

status() {
    echo "interface: $IFACE"
    $CLI -i "$IFACE" status 2>/dev/null | grep -E 'wpa_state|ssid|ip_address' || true
    running "$PID_DIR/player.pid" && echo "cedar: running" || echo "cedar: stopped"
    running "$PID_DIR/ffmpeg.pid" && echo "ffmpeg: running" || echo "ffmpeg: stopped"
    [ -p "$FIFO" ] && echo "fifo: $FIFO" || echo "fifo: absent"
}

case "${1:-}" in
    start) start ;;
    stop) stop ;;
    restart) stop; start ;;
    status) status ;;
    *) echo "Usage: $0 {start|stop|restart|status}"; exit 2 ;;
esac

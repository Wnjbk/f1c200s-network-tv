#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "driver/drm_warpper.h"
#include "render/mediaplayer.h"
#include "utils/log.h"
#include "utils/misc.h"

drm_warpper_t g_drm_warpper;
mediaplayer_t g_mediaplayer;
buffer_object_t g_video_buf;
int g_video_buf_ready = 0;
static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

int main(int argc, char **argv)
{
    const char *path;
    int raw_h264 = 0;
    int raw_width = 0;
    int raw_height = 0;
    int raw_fps = 30;

    if (argc == 2) {
        path = argv[1];
    } else if (argc == 6 && strcmp(argv[1], "--raw-h264") == 0) {
        raw_h264 = 1;
        raw_width = atoi(argv[2]);
        raw_height = atoi(argv[3]);
        raw_fps = atoi(argv[4]);
        path = argv[5];
        if (raw_width <= 0 || raw_height <= 0 || raw_fps <= 0) {
            fprintf(stderr, "invalid raw h264 args\n");
            return 2;
        }
    } else {
        fprintf(stderr, "usage: %s /path/video.mp4\n", argv[0]);
        fprintf(stderr, "       %s --raw-h264 WIDTH HEIGHT FPS /path/stream.h264\n", argv[0]);
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (drm_warpper_init(&g_drm_warpper) != 0) {
        log_error("drm init failed");
        return 1;
    }

    if (mediaplayer_init(&g_mediaplayer, &g_drm_warpper) != 0) {
        log_error("mediaplayer init failed");
        drm_warpper_destroy(&g_drm_warpper);
        return 1;
    }

    if ((!raw_h264 &&
         (mediaplayer_set_video(&g_mediaplayer, path) != 0 ||
          mediaplayer_start(&g_mediaplayer) != 0)) ||
        (raw_h264 &&
         mediaplayer_start_raw_h264(&g_mediaplayer, path,
                                    raw_width, raw_height, raw_fps) != 0)) {
        log_error("playback start failed: %s", path);
        mediaplayer_destroy(&g_mediaplayer);
        drm_warpper_destroy(&g_drm_warpper);
        return 1;
    }

    log_info("playing %s", path);
    while (g_running && mediaplayer_get_status(&g_mediaplayer) == MP_STATUS_PLAYING) {
        sleep(1);
    }

    mediaplayer_stop(&g_mediaplayer);
    mediaplayer_destroy(&g_mediaplayer);
    drm_warpper_destroy(&g_drm_warpper);
    return 0;
}

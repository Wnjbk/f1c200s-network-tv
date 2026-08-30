#include "mediaplayer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <CdxParser.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <vdecoder.h>
#include <memoryAdapter.h>


#include "driver/drm_warpper.h"
#include "utils/log.h"
#include "cdx_config.h"
#include "config.h"
#include "driver/srgn_drm.h"
#include "utils/misc.h"

/* external cedarx plugin entry */
extern void AddVDPlugin(void);
extern buffer_object_t g_video_buf;
extern int g_video_buf_ready;

#define mp_get_now_us get_now_us
#define MP_AUDIO_QUEUE_MAX_BYTES (512 * 1024)
#define MP_AUDIO_QUEUE_MAX_PACKETS 128
#define MP_AUDIO_PCM_SAMPLES 8192
#define MP_MIN_FRAME_INTERVAL_US 5000
#define MP_MAX_FRAME_INTERVAL_US 250000
#define MP_RAW_READ_CHUNK (32 * 1024)
#define MP_RAW_BUFFER_MAX (1024 * 1024)

static int mp_is_network_uri(const char *path)
{
    return path &&
           (strncmp(path, "http://", 7) == 0 ||
            strncmp(path, "https://", 8) == 0 ||
            strncmp(path, "rtsp://", 7) == 0);
}

static int64_t mp_frame_interval_us(const mediaplayer_t *mp)
{
    int64_t interval;

    if (mp->framerate > 0) {
        if (mp->framerate > 240) {
            interval = 1000000LL * 1000LL / mp->framerate;
        } else {
            interval = 1000000LL / mp->framerate;
        }
        if (interval >= MP_MIN_FRAME_INTERVAL_US &&
            interval <= MP_MAX_FRAME_INTERVAL_US) {
            return interval;
        }
    }
    return 33333;
}

static int mp_sane_frame_interval(int64_t interval)
{
    return interval >= MP_MIN_FRAME_INTERVAL_US &&
           interval <= MP_MAX_FRAME_INTERVAL_US;
}

static int mp_env_int(const char *name, int fallback)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return fallback;
    int n = atoi(v);
    return n > 0 ? n : fallback;
}


static int mp_env_flag(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

static void mp_configure_yuv_view_once(mediaplayer_t *mp, VideoPicture *picture)
{
    if (mp->yuv_view_configured || !picture)
        return;

    int src_w = picture->nWidth;
    int src_h = picture->nHeight;
    int safe_x = mp_env_int("CEDAR_VIEW_X", 12);
    int safe_y = mp_env_int("CEDAR_VIEW_Y", 0);
    int safe_w = mp_env_int("CEDAR_VIEW_W", 360);
    int safe_h = mp_env_int("CEDAR_VIEW_H", 640);
    int stretch = mp_env_int("CEDAR_VIEW_STRETCH", 0);
    int out_w = safe_w;
    int out_h = safe_h;
    int x = safe_x;
    int y = safe_y;

    if (!stretch) {
        long long fit_h = ((long long)safe_w * src_h) / src_w;
        long long fit_w = ((long long)safe_h * src_w) / src_h;
        if (fit_h <= safe_h) {
            out_w = safe_w;
            out_h = (int)fit_h;
        } else {
            out_w = (int)fit_w;
            out_h = safe_h;
        }
        if (out_w < 2)
            out_w = 2;
        if (out_h < 2)
            out_h = 2;
        out_w &= ~1;
        out_h &= ~1;
        x = safe_x + (safe_w - out_w) / 2;
        y = safe_y + (safe_h - out_h) / 2;
    }

    drm_warpper_set_yuv_view(mp->drm_warpper, DRM_WARPPER_LAYER_VIDEO,
                             src_w, src_h, out_w, out_h, x, y);
    mp->yuv_view_configured = 1;
}

static void audio_queue_init(AudioQueue *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void audio_queue_clear_locked(AudioQueue *q)
{
    AudioPacket *pkt = q->head;
    while (pkt) {
        AudioPacket *next = pkt->next;
        free(pkt->data);
        free(pkt);
        pkt = next;
    }
    q->head = NULL;
    q->tail = NULL;
    q->packets = 0;
    q->bytes = 0;
}

static void audio_queue_destroy(AudioQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    audio_queue_clear_locked(q);
    pthread_mutex_unlock(&q->mutex);
    pthread_cond_destroy(&q->cond);
    pthread_mutex_destroy(&q->mutex);
}

static void audio_queue_reset(AudioQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    audio_queue_clear_locked(q);
    q->eof = 0;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static void mp_reset_thread_state(mediaplayer_t *mp)
{
    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.end_of_stream = 0;
    mp->thread.state = 0;
    mp->thread.requested_stop = 0;
    pthread_rwlock_unlock(&mp->thread.rwlock);
    audio_queue_reset(&mp->audio_queue);
    mp->video_base_pts = 0;
    mp->video_base_time = 0;
    mp->video_last_pts = -1;
    mp->video_clock_inited = 0;
}

static int audio_queue_push(mediaplayer_t *mp, const CdxPacketT *packet)
{
    AudioPacket *item = calloc(1, sizeof(*item));
    if (!item) {
        return -1;
    }

    item->data = malloc(packet->length);
    if (!item->data) {
        free(item);
        return -1;
    }

    memcpy(item->data, packet->buf, packet->length);
    item->length = packet->length;
    item->pts = packet->pts;

    pthread_mutex_lock(&mp->audio_queue.mutex);
    while (!mp->thread.requested_stop &&
           (mp->audio_queue.bytes > MP_AUDIO_QUEUE_MAX_BYTES ||
            mp->audio_queue.packets > MP_AUDIO_QUEUE_MAX_PACKETS)) {
        pthread_cond_wait(&mp->audio_queue.cond, &mp->audio_queue.mutex);
    }

    if (mp->thread.requested_stop) {
        pthread_mutex_unlock(&mp->audio_queue.mutex);
        free(item->data);
        free(item);
        return -1;
    }

    if (mp->audio_queue.tail) {
        mp->audio_queue.tail->next = item;
    } else {
        mp->audio_queue.head = item;
    }
    mp->audio_queue.tail = item;
    mp->audio_queue.packets++;
    mp->audio_queue.bytes += item->length;
    pthread_cond_broadcast(&mp->audio_queue.cond);
    pthread_mutex_unlock(&mp->audio_queue.mutex);
    return 0;
}

static AudioPacket *audio_queue_pop(mediaplayer_t *mp)
{
    pthread_mutex_lock(&mp->audio_queue.mutex);
    while (!mp->thread.requested_stop && !mp->audio_queue.eof && !mp->audio_queue.head) {
        pthread_cond_wait(&mp->audio_queue.cond, &mp->audio_queue.mutex);
    }

    AudioPacket *item = mp->audio_queue.head;
    if (item) {
        mp->audio_queue.head = item->next;
        if (!mp->audio_queue.head) {
            mp->audio_queue.tail = NULL;
        }
        mp->audio_queue.packets--;
        mp->audio_queue.bytes -= item->length;
    }
    pthread_cond_broadcast(&mp->audio_queue.cond);
    pthread_mutex_unlock(&mp->audio_queue.mutex);
    return item;
}

static void audio_queue_set_eof(AudioQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    q->eof = 1;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static int alsa_open_pcm(snd_pcm_t **pcm_out, int sample_rate, int channels)
{
    const char *dev = getenv("CEDAR_AUDIODEV");
    if (!dev || !dev[0]) {
        dev = getenv("AUDIODEV");
    }
    if (!dev || !dev[0]) {
        dev = "hw:2,0";
    }

    snd_pcm_t *pcm = NULL;
    int ret = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        log_error("snd_pcm_open %s failed: %s", dev, snd_strerror(ret));
        return -1;
    }

    unsigned int rate = sample_rate;
    ret = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             channels, rate, 1, 80000);
    if (ret < 0) {
        log_error("snd_pcm_set_params %s failed: %s", dev, snd_strerror(ret));
        snd_pcm_close(pcm);
        return -1;
    }

    log_info("audio output: %s %d Hz %d ch", dev, sample_rate, channels);
    *pcm_out = pcm;
    return 0;
}

static int alsa_write_all(snd_pcm_t *pcm, const INT_PCM *samples, int frames, int channels)
{
    const INT_PCM *p = samples;
    int left = frames;
    while (left > 0) {
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, p, left);
        if (written < 0) {
            written = snd_pcm_recover(pcm, written, 1);
        }
        if (written < 0) {
            log_error("snd_pcm_writei failed: %s", snd_strerror(written));
            return -1;
        }
        if (written == 0) {
            usleep(1000);
            continue;
        }
        p += written * channels;
        left -= written;
    }
    return 0;
}

static void *mp_audio_thread(void *param)
{
    mediaplayer_t *mp = (mediaplayer_t *)param;
    struct CdxProgramS *program = &mp->media_info.program[mp->media_info.programIndex];
    AudioStreamInfo *audio = &program->audio[program->audioIndex >= 0 ? program->audioIndex : 0];
    HANDLE_AACDECODER dec = NULL;
    snd_pcm_t *pcm = NULL;
    INT_PCM *pcm_buf = NULL;
    int pcm_opened = 0;
    int output_channels = audio->nChannelNum > 0 ? audio->nChannelNum : 2;
    int output_rate = audio->nSampleRate > 0 ? audio->nSampleRate : 44100;

    log_info("==> mp_audio Thread Started!");
    log_info("audio stream codec=%d rate=%d channels=%d extradata=%d",
             audio->eCodecFormat, audio->nSampleRate, audio->nChannelNum,
             audio->nCodecSpecificDataLen);

    if (audio->eCodecFormat != AUDIO_CODEC_FORMAT_MPEG_AAC_LC &&
        audio->eCodecFormat != AUDIO_CODEC_FORMAT_RAAC) {
        log_warn("unsupported integrated audio codec: %d", audio->eCodecFormat);
        goto audio_exit;
    }

    dec = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!dec) {
        log_error("aacDecoder_Open failed");
        goto audio_exit;
    }

    if (audio->pCodecSpecificData && audio->nCodecSpecificDataLen > 0) {
        UCHAR *conf[] = {(UCHAR *)audio->pCodecSpecificData};
        UINT conf_len[] = {(UINT)audio->nCodecSpecificDataLen};
        AAC_DECODER_ERROR err = aacDecoder_ConfigRaw(dec, conf, conf_len);
        if (err != AAC_DEC_OK) {
            log_error("aacDecoder_ConfigRaw failed: 0x%x", err);
            goto audio_exit;
        }
    }

    pcm_buf = malloc(sizeof(INT_PCM) * MP_AUDIO_PCM_SAMPLES);
    if (!pcm_buf) {
        log_error("audio pcm malloc failed");
        goto audio_exit;
    }

    while (1) {
        pthread_rwlock_rdlock(&mp->thread.rwlock);
        int requested_stop = mp->thread.requested_stop;
        pthread_rwlock_unlock(&mp->thread.rwlock);
        if (requested_stop) {
            break;
        }

        AudioPacket *pkt = audio_queue_pop(mp);
        if (!pkt) {
            if (mp->audio_queue.eof) {
                break;
            }
            continue;
        }

        UCHAR *in_buf[] = {pkt->data};
        UINT in_size[] = {(UINT)pkt->length};
        UINT valid = (UINT)pkt->length;
        AAC_DECODER_ERROR err = aacDecoder_Fill(dec, in_buf, in_size, &valid);
        if (err != AAC_DEC_OK) {
            log_warn("aacDecoder_Fill failed: 0x%x", err);
            free(pkt->data);
            free(pkt);
            continue;
        }

        do {
            err = aacDecoder_DecodeFrame(dec, pcm_buf, MP_AUDIO_PCM_SAMPLES, 0);
            if (err == AAC_DEC_NOT_ENOUGH_BITS) {
                break;
            }
            if (err != AAC_DEC_OK) {
                log_warn("aacDecoder_DecodeFrame failed: 0x%x", err);
                break;
            }

            CStreamInfo *info = aacDecoder_GetStreamInfo(dec);
            if (!info || info->frameSize <= 0 || info->numChannels <= 0) {
                continue;
            }

            if (!pcm_opened ||
                output_rate != info->sampleRate ||
                output_channels != info->numChannels) {
                if (pcm) {
                    snd_pcm_drain(pcm);
                    snd_pcm_close(pcm);
                    pcm = NULL;
                }
                output_rate = info->sampleRate;
                output_channels = info->numChannels;
                if (alsa_open_pcm(&pcm, output_rate, output_channels) != 0) {
                    goto audio_exit_packet;
                }
                pcm_opened = 1;
            }

            int samples = info->frameSize * info->numChannels;
            if (samples > MP_AUDIO_PCM_SAMPLES) {
                log_warn("decoded pcm too large: %d", samples);
                continue;
            }
            if (alsa_write_all(pcm, pcm_buf, info->frameSize, info->numChannels) != 0) {
                goto audio_exit_packet;
            }
        } while (valid == 0);

        free(pkt->data);
        free(pkt);
        continue;

audio_exit_packet:
        free(pkt->data);
        free(pkt);
        break;
    }

audio_exit:
    if (pcm) {
        snd_pcm_drain(pcm);
        snd_pcm_close(pcm);
    }
    if (dec) {
        aacDecoder_Close(dec);
    }
    free(pcm_buf);

    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.state |= MEDIAPLAYER_AUDIO_EXIT;
    pthread_rwlock_unlock(&mp->thread.rwlock);
    log_info("==> mp_audio Thread Ended!");
    pthread_exit(NULL);
    return NULL;
}

/* parser thread: read bitstream and feed decoder */
static void *mp_parser_thread(void *param)
{
    mediaplayer_t *mp = (mediaplayer_t *)param;
    CdxParserT *parser = mp->parser;
    VideoDecoder *decoder = mp->decoder;
    CdxPacketT packet;
    VideoStreamDataInfo dataInfo;
    int ret;
    int validSize;
    int requestSize;
    int streamNum;
    int trytime = 0;
    unsigned char *buf = NULL;
    int buf_size = 1024 * 1024;

    buf = malloc(buf_size);
    if (buf == NULL) {
        log_error("parser thread malloc err");
        goto parser_exit;
    }

    memset(&packet, 0, sizeof(packet));
    memset(&dataInfo, 0, sizeof(dataInfo));

    log_info("==> mp_parser Thread Started!");

    startagain:
    while (0 == CdxParserPrefetch(parser, &packet)) {
        usleep(50);

        pthread_rwlock_rdlock(&mp->thread.rwlock);
        int state = mp->thread.state;
        int requested_stop = mp->thread.requested_stop;
        pthread_rwlock_unlock(&mp->thread.rwlock);

        if (requested_stop || (state & (MEDIAPLAYER_PARSER_ERROR |
                                        MEDIAPLAYER_DECODER_ERROR |
                                        MEDIAPLAYER_DECODE_FINISH))) {
            // log_info("parser:get exit flag");
            break;
        }

        validSize = VideoStreamBufferSize(decoder, 0) - VideoStreamDataSize(decoder, 0);
        requestSize = packet.length;

        if (trytime >= 2000) {
            log_error("try time too much");
            pthread_rwlock_wrlock(&mp->thread.rwlock);
            mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
            pthread_rwlock_unlock(&mp->thread.rwlock);
            break;
        }

        if (packet.type == CDX_MEDIA_VIDEO && ((packet.flags & MINOR_STREAM) == 0)) {
            if (requestSize > validSize) {
                usleep(50 * 1000);
                trytime++;
                continue;
            }

            ret = RequestVideoStreamBuffer(decoder, requestSize,
                                           (char **)&packet.buf, &packet.buflen,
                                           (char **)&packet.ringBuf, &packet.ringBufLen, 0);
            if (ret != 0) {
                log_debug("RequestVideoStreamBuffer err, request=%d, valid=%d",
                          requestSize, validSize);
                usleep(50 * 1000);
                continue;
            }

            if (packet.buflen + packet.ringBufLen < requestSize) {
                log_error("RequestVideoStreamBuffer err, not enough space");
                pthread_rwlock_wrlock(&mp->thread.rwlock);
                mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
                pthread_rwlock_unlock(&mp->thread.rwlock);
                break;
            }
        } else if (packet.type == CDX_MEDIA_AUDIO && mp->has_audio) {
            if (requestSize > buf_size) {
                unsigned char *new_buf = realloc(buf, requestSize);
                if (!new_buf) {
                    log_error("audio temp realloc failed: %d", requestSize);
                    pthread_rwlock_wrlock(&mp->thread.rwlock);
                    mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
                    pthread_rwlock_unlock(&mp->thread.rwlock);
                    break;
                }
                buf = new_buf;
                buf_size = requestSize;
            }
            packet.buf = buf;
            packet.buflen = requestSize;
            ret = CdxParserRead(parser, &packet);
            if (ret != 0) {
                log_error("cdxparser read audio err");
                pthread_rwlock_wrlock(&mp->thread.rwlock);
                mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
                pthread_rwlock_unlock(&mp->thread.rwlock);
                break;
            }
            if (audio_queue_push(mp, &packet) != 0) {
                log_error("audio queue push err");
                pthread_rwlock_wrlock(&mp->thread.rwlock);
                mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
                pthread_rwlock_unlock(&mp->thread.rwlock);
                break;
            }
            continue;
        } else {
            if (requestSize > buf_size) {
                unsigned char *new_buf = realloc(buf, requestSize);
                if (!new_buf) {
                    log_error("parser temp realloc failed: %d", requestSize);
                    pthread_rwlock_wrlock(&mp->thread.rwlock);
                    mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
                    pthread_rwlock_unlock(&mp->thread.rwlock);
                    break;
                }
                buf = new_buf;
                buf_size = requestSize;
            }
            packet.buf = buf;
            packet.buflen = packet.length;
            CdxParserRead(parser, &packet);
            continue;
        }

        trytime = 0;
        streamNum = VideoStreamFrameNum(decoder, 0);
        if (streamNum > 1024) {
            usleep(50 * 1000);
        }

        ret = CdxParserRead(parser, &packet);
        if (ret != 0) {
            log_error("cdxparser read err");
            pthread_rwlock_wrlock(&mp->thread.rwlock);
            mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
            pthread_rwlock_unlock(&mp->thread.rwlock);
            break;
        }

        memset(&dataInfo, 0, sizeof(dataInfo));
        dataInfo.pData        = packet.buf;
        dataInfo.nLength      = packet.length;
        dataInfo.nPts         = packet.pts;
        dataInfo.nPcr         = packet.pcr;
        dataInfo.bIsFirstPart = !!(packet.flags & FIRST_PART);
        dataInfo.bIsLastPart  = !!(packet.flags & LAST_PART);

        ret = SubmitVideoStreamData(decoder, &dataInfo, 0);
        if (ret != 0) {
            log_error("SubmitVideoStreamData err");
            pthread_rwlock_wrlock(&mp->thread.rwlock);
            mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
            pthread_rwlock_unlock(&mp->thread.rwlock);
            break;
        }
    }
    
    if(CdxParserGetStatus(parser) == PSR_EOS){
        log_debug("eos, start again!");
        audio_queue_reset(&mp->audio_queue);
        CdxParserSeekTo(parser, 0, AW_SEEK_CLOSEST);  
        goto startagain;
    }

    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.end_of_stream = 1;
    mp->thread.state |= MEDIAPLAYER_PARSER_EXIT;
    pthread_rwlock_unlock(&mp->thread.rwlock);
    audio_queue_set_eof(&mp->audio_queue);

parser_exit:
    if (buf) {
        free(buf);
    }
    log_info("==> mp_parser Thread Ended!");
    pthread_exit(NULL);
    return NULL;
}

/* raw thread: read Annex-B H.264 from file/FIFO and feed decoder */
static void *mp_raw_h264_thread(void *param)
{
    mediaplayer_t *mp = (mediaplayer_t *)param;
    VideoDecoder *decoder = mp->decoder;
    int fd = -1;
    unsigned char *stream_buf = NULL;
    int stream_len = 0;
    int stream_cap = MP_RAW_BUFFER_MAX;
    int64_t pts = 0;
    int64_t pts_step = mp_frame_interval_us(mp);

    log_info("==> mp_raw_h264 Thread Started!");

    stream_buf = malloc(stream_cap);
    if (!stream_buf) {
        log_error("raw h264 buffer malloc failed");
        pthread_rwlock_wrlock(&mp->thread.rwlock);
        mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
        pthread_rwlock_unlock(&mp->thread.rwlock);
        goto raw_exit;
    }

    fd = open(mp->video_path, O_RDONLY);
    if (fd < 0) {
        log_error("open raw h264 failed: %s errno=%d", mp->video_path, errno);
        pthread_rwlock_wrlock(&mp->thread.rwlock);
        mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
        pthread_rwlock_unlock(&mp->thread.rwlock);
        goto raw_exit;
    }

    while (1) {
        int state;
        int requested_stop;
        ssize_t nread;

        pthread_rwlock_rdlock(&mp->thread.rwlock);
        state = mp->thread.state;
        requested_stop = mp->thread.requested_stop;
        pthread_rwlock_unlock(&mp->thread.rwlock);

        if (requested_stop || (state & (MEDIAPLAYER_PARSER_ERROR |
                                        MEDIAPLAYER_DECODER_ERROR))) {
            break;
        }

        if (stream_cap - stream_len < MP_RAW_READ_CHUNK) {
            log_warn("raw h264 buffer full, dropping %d bytes before next read", stream_len);
            stream_len = 0;
        }

        nread = read(fd, stream_buf + stream_len, MP_RAW_READ_CHUNK);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("read raw h264 failed errno=%d", errno);
            pthread_rwlock_wrlock(&mp->thread.rwlock);
            mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
            pthread_rwlock_unlock(&mp->thread.rwlock);
            break;
        }
        if (nread == 0) {
            log_info("raw h264 input eos");
            break;
        }
        stream_len += (int)nread;

        for (;;) {
            int start = -1;
            int next = -1;
            int i;

            for (i = 0; i + 3 < stream_len; i++) {
                if (stream_buf[i] == 0 && stream_buf[i + 1] == 0 &&
                    ((stream_buf[i + 2] == 1) ||
                     (stream_buf[i + 2] == 0 && stream_buf[i + 3] == 1))) {
                    start = i;
                    break;
                }
            }

            if (start < 0) {
                if (stream_len > 4) {
                    memmove(stream_buf, stream_buf + stream_len - 4, 4);
                    stream_len = 4;
                }
                break;
            }

            if (start > 0) {
                memmove(stream_buf, stream_buf + start, stream_len - start);
                stream_len -= start;
                start = 0;
            }

            for (i = 3; i + 3 < stream_len; i++) {
                if (stream_buf[i] == 0 && stream_buf[i + 1] == 0 &&
                    ((stream_buf[i + 2] == 1) ||
                     (stream_buf[i + 2] == 0 && stream_buf[i + 3] == 1))) {
                    next = i;
                    break;
                }
            }

            if (next < 0) {
                break;
            }

            int packet_len = next;
            int trytime = 0;
            while (1) {
                int validSize;
                char *buf0 = NULL;
                char *buf1 = NULL;
                int len0 = 0;
                int len1 = 0;
                int ret;
                VideoStreamDataInfo dataInfo;

                validSize = VideoStreamBufferSize(decoder, 0) - VideoStreamDataSize(decoder, 0);
                if (packet_len > validSize) {
                    if (++trytime >= 2000) {
                        log_error("raw h264 decoder buffer wait timeout");
                        pthread_rwlock_wrlock(&mp->thread.rwlock);
                        mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
                        pthread_rwlock_unlock(&mp->thread.rwlock);
                        goto raw_done;
                    }
                    usleep(20 * 1000);
                    continue;
                }

                ret = RequestVideoStreamBuffer(decoder, packet_len,
                                               &buf0, &len0, &buf1, &len1, 0);
                if (ret != 0 || !buf0 || len0 + len1 < packet_len) {
                    usleep(20 * 1000);
                    continue;
                }

                if (packet_len <= len0) {
                    memcpy(buf0, stream_buf, packet_len);
                } else {
                    memcpy(buf0, stream_buf, len0);
                    memcpy(buf1, stream_buf + len0, packet_len - len0);
                }

                memset(&dataInfo, 0, sizeof(dataInfo));
                dataInfo.pData        = buf0;
                dataInfo.nLength      = packet_len;
                dataInfo.nPts         = pts;
                dataInfo.nPcr         = -1;
                dataInfo.bIsFirstPart = 1;
                dataInfo.bIsLastPart  = 1;

                ret = SubmitVideoStreamData(decoder, &dataInfo, 0);
                if (ret != 0) {
                    log_error("SubmitVideoStreamData raw h264 err");
                    pthread_rwlock_wrlock(&mp->thread.rwlock);
                    mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
                    pthread_rwlock_unlock(&mp->thread.rwlock);
                    goto raw_done;
                }
                pts += pts_step;
                break;
            }

            memmove(stream_buf, stream_buf + packet_len, stream_len - packet_len);
            stream_len -= packet_len;
        }
    }

raw_done:
    if (stream_len > 4) {
        int validSize = VideoStreamBufferSize(decoder, 0) - VideoStreamDataSize(decoder, 0);
        if (stream_len <= validSize) {
            char *buf0 = NULL;
            char *buf1 = NULL;
            int len0 = 0;
            int len1 = 0;
            if (RequestVideoStreamBuffer(decoder, stream_len,
                                         &buf0, &len0, &buf1, &len1, 0) == 0 &&
                buf0 && len0 + len1 >= stream_len) {
                VideoStreamDataInfo dataInfo;
                if (stream_len <= len0) {
                    memcpy(buf0, stream_buf, stream_len);
                } else {
                    memcpy(buf0, stream_buf, len0);
                    memcpy(buf1, stream_buf + len0, stream_len - len0);
                }
                memset(&dataInfo, 0, sizeof(dataInfo));
                dataInfo.pData        = buf0;
                dataInfo.nLength      = stream_len;
                dataInfo.nPts         = pts;
                dataInfo.nPcr         = -1;
                dataInfo.bIsFirstPart = 1;
                dataInfo.bIsLastPart  = 1;
                SubmitVideoStreamData(decoder, &dataInfo, 0);
            }
        }
    }

    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.end_of_stream = 1;
    mp->thread.state |= MEDIAPLAYER_PARSER_EXIT;
    pthread_rwlock_unlock(&mp->thread.rwlock);

raw_exit:
    if (fd >= 0) {
        close(fd);
    }
    free(stream_buf);
    audio_queue_set_eof(&mp->audio_queue);
    log_info("==> mp_raw_h264 Thread Ended!");
    pthread_exit(NULL);
    return NULL;
}

/* decoder thread: decode and copy one frame to output buffer */
static void *mp_decoder_thread(void *param)
{
    mediaplayer_t *mp = (mediaplayer_t *)param;
    VideoDecoder *decoder = mp->decoder;
    int ret;
    int end_of_stream = 0;
    long long next_frame_time = 0;
    int64_t fallback_interval = mp_frame_interval_us(mp);


    next_frame_time = mp_get_now_us() + fallback_interval;

    log_info("==> mp_decoder Thread Started!");
    log_info("==> target fps: %d interval=%lld us", mp->framerate,
             (long long)fallback_interval);

    while (1) {
        usleep(50);

        pthread_rwlock_rdlock(&mp->thread.rwlock);
        end_of_stream = mp->thread.end_of_stream;
        int state = mp->thread.state;
        int requested_stop = mp->thread.requested_stop;
        pthread_rwlock_unlock(&mp->thread.rwlock);

        if (requested_stop) {
            // log_info("req stop,exiting");
            break;
        }

        if (state & (MEDIAPLAYER_PARSER_ERROR | MEDIAPLAYER_DECODER_ERROR)) {
            log_error("err state,exiting");
            break;
        }

        // first try to dequeue free buffer from drm_warpper
        drm_warpper_queue_item_t* item;
        while(drm_warpper_try_dequeue_free_item(mp->drm_warpper, DRM_WARPPER_LAYER_VIDEO, &item) == 0){
            VideoPicture* pic = (VideoPicture*)item->userdata;
            // log_debug("dequeue");
            if(pic){
                ReturnPicture(decoder, pic);
            }
            free(item);
        }

        // long long start = mp_get_now_us();
        ret = DecodeVideoStream(decoder, end_of_stream, 0, 0, 0);
        // long long finish = mp_get_now_us();
        // log_debug("frame time: %lld us", finish - start);

        if (end_of_stream == 1 && ret == VDECODE_RESULT_NO_BITSTREAM) {
            log_info("data end!!!");
            break;
        }

        if (ret == VDECODE_RESULT_KEYFRAME_DECODED ||
            ret == VDECODE_RESULT_FRAME_DECODED) {
            int validNum = ValidPictureNum(decoder, 0);
            if (validNum > 0) {
                VideoPicture *picture = RequestPicture(decoder, 0);
                if (!picture) {
                    log_error("RequestPicture err");
                    continue;
                }

                if (picture->nWidth != (int)g_video_buf.width ||
                    picture->nHeight != (int)g_video_buf.height) {
                    log_error("err size, expect %dx%d, actual %dx%d",
                              g_video_buf.width, g_video_buf.height,
                              picture->nWidth, picture->nHeight);
                    ReturnPicture(decoder, picture);
                    pthread_rwlock_wrlock(&mp->thread.rwlock);
                    mp->thread.state |= MEDIAPLAYER_DECODER_ERROR;
                    pthread_rwlock_unlock(&mp->thread.rwlock);
                    break;
                }

                mp_configure_yuv_view_once(mp, picture);

                int64_t now = mp_get_now_us();
                int64_t target_time = now;
                int64_t frame_interval = fallback_interval;

                if (!mp->video_clock_inited) {
                    mp->video_base_pts = picture->nPts;
                    mp->video_base_time = now;
                    mp->video_last_pts = picture->nPts;
                    mp->video_clock_inited = 1;
                    next_frame_time = now;
                } else {
                    if (picture->nPts >= 0 && mp->video_last_pts >= 0 &&
                        picture->nPts >= mp->video_last_pts) {
                        int64_t pts_delta = picture->nPts - mp->video_last_pts;
                        if (mp_sane_frame_interval(pts_delta)) {
                            frame_interval = pts_delta;
                        }
                    }
                    target_time = next_frame_time + frame_interval;
                    if (picture->nPts >= 0) {
                        mp->video_last_pts = picture->nPts;
                    }
                    next_frame_time = target_time;
                }

                now = mp_get_now_us();
                if (!mp_env_flag("CEDAR_NO_PACE")) {
                    if (target_time > now) {
                        int64_t wait_us = target_time - now;
                        if (wait_us <= MP_MAX_FRAME_INTERVAL_US) {
                            usleep(wait_us);
                        } else {
                            next_frame_time = now + fallback_interval;
                        }
                    } else if (now - target_time > 2 * 1000 * 1000) {
                        log_warn("video clock late: %lld us", (long long)(now - target_time));
                        next_frame_time = now;
                    }
                } else {
                    next_frame_time = now;
                }

                // int dataLen = picture->nWidth * picture->nHeight * 3 / 2;
                // memcpy(mp->output_buf, picture->pData0,
                //        picture->nWidth * picture->nHeight);
                // memcpy(mp->output_buf + picture->nWidth * picture->nHeight,
                //        picture->pData1,
                //        picture->nWidth * picture->nHeight / 2);

                // ReturnPicture(decoder, picture);

                // 把拿到的picture直接交给drm ioctl挂上去(Zero Copy!)
                drm_warpper_queue_item_t* item_to_display = malloc(sizeof(drm_warpper_queue_item_t));
                if(item_to_display == NULL){
                    log_error("malloc err");
                    ReturnPicture(decoder, picture);
                    continue;
                }

                item_to_display->mount.type = DRM_SRGN_ATOMIC_COMMIT_MOUNT_FB_YUV;
                item_to_display->mount.arg0 = (uint32_t)picture->pData0;
                item_to_display->mount.arg1 = (uint32_t)picture->pData1;
                item_to_display->mount.arg2 = 0;
                item_to_display->userdata = (void*)picture;
                // this "on_heap" means that the item_to_display 
                // will be free by the drm_warpper, not by the mediaplayer.
                item_to_display->on_heap = false;
                ret = drm_warpper_try_enqueue_display_item(mp->drm_warpper, DRM_WARPPER_LAYER_VIDEO, item_to_display);
                if (ret != 0) {
                    static unsigned int dropped_display_frames;
                    dropped_display_frames++;
                    if ((dropped_display_frames & 0x3f) == 1) {
                        log_warn("display queue full, drop frame count=%u", dropped_display_frames);
                    }
                    ReturnPicture(decoder, picture);
                    free(item_to_display);
                }
            }
        }

        if (ret < 0) {
            log_error("DecodeVideoStream err: %d", ret);
            pthread_rwlock_wrlock(&mp->thread.rwlock);
            mp->thread.state |= MEDIAPLAYER_DECODER_ERROR;
            pthread_rwlock_unlock(&mp->thread.rwlock);
            break;
        }
    }

    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.state |= MEDIAPLAYER_DECODER_EXIT;
    pthread_rwlock_unlock(&mp->thread.rwlock);

    log_info("==> mp_decoder Thread Ended!");
    pthread_exit(NULL);
    return NULL;
}

/* internal helper: release parser/decoder/memops */
static void mp_cleanup_internal(mediaplayer_t *mp)
{
    if (mp->parser) {
        CdxParserClose(mp->parser);
        mp->parser = NULL;
    }
    if (mp->decoder) {
        DestroyVideoDecoder(mp->decoder);
        mp->decoder = NULL;
    }
    if (mp->memops) {
        CdcMemClose(mp->memops);
        mp->memops = NULL;
    }
}

int mediaplayer_init(mediaplayer_t *mp,drm_warpper_t *drm_warpper)
{

    memset(mp, 0, sizeof(*mp));

    pthread_mutex_init(&mp->parser_mutex, NULL);
    pthread_rwlock_init(&mp->thread.rwlock, NULL);
    mp->thread.end_of_stream = 0;
    mp->thread.state = 0;
    mp->thread.requested_stop = 0;
    atomic_store(&mp->running, 0);
    memset(mp->video_path, 0, sizeof(mp->video_path));

    mp->drm_warpper = drm_warpper;
    audio_queue_init(&mp->audio_queue);

    AddVDPlugin();

    log_info("==> mp Initalized!");
    return 0;
}

int mediaplayer_destroy(mediaplayer_t *mp)
{
    if (!mp) {
        return -1;
    }

    /* ensure stopped */
    mediaplayer_stop(mp);

    mp_cleanup_internal(mp);

    pthread_rwlock_destroy(&mp->thread.rwlock);
    pthread_mutex_destroy(&mp->parser_mutex);
    audio_queue_destroy(&mp->audio_queue);

    return 0;
}

int mediaplayer_play_video(mediaplayer_t *mp, const char *file)
{
    if (!mp || !file) {
        log_error("invalid params");
        return -1;
    }

    if (atomic_load(&mp->running)) {
        log_error("mediaplayer is running");
        return -1;
    }

    // 每次开始之前都需要reset cache
    drm_warpper_reset_cache_ioctl(mp->drm_warpper);

    memset(mp->input_uri, 0, sizeof(mp->input_uri));
    snprintf(mp->input_uri, sizeof(mp->input_uri), "file://%s", file);

    mp->memops = MemAdapterGetOpsS();
    if (!mp->memops) {
        log_error("MemAdapterGetOpsS err");
        return -1;
    }
    CdcMemOpen(mp->memops);

    memset(&mp->source, 0, sizeof(CdxDataSourceT));
    memset(&mp->media_info, 0, sizeof(CdxMediaInfoT));

    mp->source.uri = mp->input_uri;

    int force_exit = 0;
    CdxStreamT *stream = NULL;
    int ret = CdxParserPrepare(&mp->source, 0, &mp->parser_mutex,
                               &force_exit, &mp->parser, &stream, NULL, NULL);
    if (ret < 0 || !mp->parser) {
        log_error("CdxParserPrepare err");
        mp_cleanup_internal(mp);
        return -1;
    }

    ret = CdxParserGetMediaInfo(mp->parser, &mp->media_info);
    if (ret != 0) {
        log_error("CdxParserGetMediaInfo err");
        mp_cleanup_internal(mp);
        return -1;
    }

    

    mp->decoder = CreateVideoDecoder();
    if (!mp->decoder) {
        log_error("CreateVideoDecoder err");
        mp_cleanup_internal(mp);
        return -1;
    }

    VConfig vConfig;
    VideoStreamInfo vInfo;
    memset(&vConfig, 0, sizeof(VConfig));
    memset(&vInfo, 0, sizeof(VideoStreamInfo));

    struct CdxProgramS *program =
        &mp->media_info.program[mp->media_info.programIndex];

    if (program->audioNum > 0) {
        if (program->audioIndex < 0 || program->audioIndex >= program->audioNum) {
            program->audioIndex = 0;
        }
        mp->has_audio = 1;
        mp->audio_sample_rate = program->audio[program->audioIndex].nSampleRate;
        mp->audio_channels = program->audio[program->audioIndex].nChannelNum;
    } else {
        mp->has_audio = 0;
    }

    /* only use first video stream */
    vInfo.eCodecFormat          = program->video[0].eCodecFormat;
    vInfo.nWidth                = program->video[0].nWidth;
    vInfo.nHeight               = program->video[0].nHeight;
    vInfo.nFrameRate            = program->video[0].nFrameRate;
    vInfo.nFrameDuration        = program->video[0].nFrameDuration;
    vInfo.nAspectRatio          = program->video[0].nAspectRatio;
    vInfo.bIs3DStream           = program->video[0].bIs3DStream;
    vInfo.nCodecSpecificDataLen = program->video[0].nCodecSpecificDataLen;
    vInfo.pCodecSpecificData    = program->video[0].pCodecSpecificData;

    mp->framerate = vInfo.nFrameRate;

    vConfig.eOutputPixelFormat  = PIXEL_FORMAT_YUV_MB32_420;
    vConfig.nDeInterlaceHoldingFrameBufferNum = BUF_CNT_4_DI;
    vConfig.nDisplayHoldingFrameBufferNum = BUF_CNT_4_LIST;
    vConfig.nRotateHoldingFrameBufferNum = BUF_CNT_4_ROTATE;
    vConfig.nDecodeSmoothFrameBufferNum = BUF_CNT_4_SMOOTH;
    vConfig.memops = mp->memops;
    vConfig.nVbvBufferSize = VBVBUFFERSIZE;

    ret = InitializeVideoDecoder(mp->decoder, &vInfo, &vConfig);
    if (ret != 0) {
        log_error("InitializeVideoDecoder err");
        mp_cleanup_internal(mp);
        return -1;
    }

    /* reset thread flags */
    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.end_of_stream = 0;
    mp->thread.state = 0;
    mp->thread.requested_stop = 0;
    pthread_rwlock_unlock(&mp->thread.rwlock);
    audio_queue_reset(&mp->audio_queue);
    mp->video_base_pts = 0;
    mp->video_base_time = 0;
    mp->video_last_pts = -1;
    mp->video_clock_inited = 0;

    atomic_store(&mp->running, 1);

    if (pthread_create(&mp->parser_thread, NULL, mp_parser_thread, mp) != 0) {
        log_error("parser create err");
        atomic_store(&mp->running, 0);
        mp_cleanup_internal(mp);
        return -1;
    }

    if (pthread_create(&mp->decoder_thread, NULL, mp_decoder_thread, mp) != 0) {
        log_error("decoder create err");
        pthread_rwlock_wrlock(&mp->thread.rwlock);
        mp->thread.requested_stop = 1;
        pthread_rwlock_unlock(&mp->thread.rwlock);
        audio_queue_set_eof(&mp->audio_queue);
        pthread_join(mp->parser_thread, NULL);
        atomic_store(&mp->running, 0);
        mp_cleanup_internal(mp);
        return -1;
    }

    if (mp->has_audio &&
        pthread_create(&mp->audio_thread, NULL, mp_audio_thread, mp) != 0) {
        log_error("audio create err");
        pthread_rwlock_wrlock(&mp->thread.rwlock);
        mp->thread.requested_stop = 1;
        pthread_rwlock_unlock(&mp->thread.rwlock);
        audio_queue_set_eof(&mp->audio_queue);
        pthread_join(mp->parser_thread, NULL);
        pthread_join(mp->decoder_thread, NULL);
        atomic_store(&mp->running, 0);
        mp_cleanup_internal(mp);
        return -1;
    }

    // /* wait for both threads to finish */
    // pthread_join(mp->parser_thread, NULL);
    // pthread_join(mp->decoder_thread, NULL);
    // mp->running = 0;

    // /* check final state */
    // pthread_rwlock_rdlock(&mp->thread.rwlock);
    // int final_state = mp->thread.state;
    // pthread_rwlock_unlock(&mp->thread.rwlock);

    // mp_cleanup_internal(mp);

    // if (final_state & (MEDIAPLAYER_PARSER_ERROR | MEDIAPLAYER_DECODER_ERROR)) {
    //     log_error("play failed, err state");
    //     return -1;
    // }
    // if (!(final_state & MEDIAPLAYER_DECODE_FINISH)) {
    //     log_error("decode failed, no frame");
    //     return -1;
    // }

    return 0;
}

int mediaplayer_stop(mediaplayer_t *mp)
{
    if (!mp) {
        return -1;
    }

    if (!atomic_load(&mp->running)) {
        return 0;
    }

    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.requested_stop = 1;
    pthread_rwlock_unlock(&mp->thread.rwlock);
    audio_queue_set_eof(&mp->audio_queue);

    pthread_join(mp->parser_thread, NULL);
    pthread_join(mp->decoder_thread, NULL);
    if (mp->has_audio) {
        pthread_join(mp->audio_thread, NULL);
    }
    atomic_store(&mp->running, 0);

    mp_cleanup_internal(mp);
    audio_queue_reset(&mp->audio_queue);
    
    // 挂载到黑屏buffer。
    drm_warpper_queue_item_t* item;
    item = malloc(sizeof(drm_warpper_queue_item_t));
    if(item == NULL){
        log_error("malloc err");
        return -1;
    }

    item->mount.type = DRM_SRGN_ATOMIC_COMMIT_MOUNT_FB_YUV;
    item->mount.arg0 = (uint32_t)g_video_buf.vaddr;
    item->mount.arg1 = (uint32_t)g_video_buf.vaddr + g_video_buf.width * g_video_buf.height;
    item->mount.arg2 = 0;
    item->userdata = NULL;
    item->on_heap = false;
    drm_warpper_enqueue_display_item(mp->drm_warpper, DRM_WARPPER_LAYER_VIDEO, item);

    return 0;
}

int mediaplayer_set_video(mediaplayer_t *mp, const char *path)
{
    if (!mp || !path) {
        log_error("invalid params");
        return -1;
    }

    if (atomic_load(&mp->running)) {
        log_error("cannot set video while playing, stop first");
        return -1;
    }

    memset(mp->video_path, 0, sizeof(mp->video_path));
    snprintf(mp->video_path, sizeof(mp->video_path), "%s", path);
    log_info("video path set to: %s", mp->video_path);

    return 0;
}

int mediaplayer_start(mediaplayer_t *mp)
{
    if (!mp) {
        log_error("invalid paramst");
        return -1;
    }

    if (atomic_load(&mp->running)) {
        log_warn("mediaplayer already running");
        return 0;
    }

    if (strlen(mp->video_path) == 0) {
        log_error("no video path set");
        return -1;
    }

    memset(mp->input_uri, 0, sizeof(mp->input_uri));
    int written;
    if (mp_is_network_uri(mp->video_path)) {
        written = snprintf(mp->input_uri, sizeof(mp->input_uri), "%s", mp->video_path);
    } else {
        written = snprintf(mp->input_uri, sizeof(mp->input_uri), "file://%s", mp->video_path);
    }
    if ((size_t)written >= sizeof(mp->input_uri)) {
        log_error("snprintf err");
        return -1;
    }
    log_info("input uri: %s", mp->input_uri);

    mp->memops = MemAdapterGetOpsS();
    if (!mp->memops) {
        log_error("MemAdapterGetOpsS err");
        return -1;
    }
    CdcMemOpen(mp->memops);

    memset(&mp->source, 0, sizeof(CdxDataSourceT));
    memset(&mp->media_info, 0, sizeof(CdxMediaInfoT));

    mp->source.uri = mp->input_uri;

    int force_exit = 0;
    CdxStreamT *stream = NULL;
    int ret = CdxParserPrepare(&mp->source, 0, &mp->parser_mutex,
                               &force_exit, &mp->parser, &stream, NULL, NULL);
    if (ret < 0 || !mp->parser) {
        log_error("CdxParserPrepare err");
        mp_cleanup_internal(mp);
        return -1;
    }

    ret = CdxParserGetMediaInfo(mp->parser, &mp->media_info);
    if (ret != 0) {
        log_error("CdxParserGetMediaInfo err");
        mp_cleanup_internal(mp);
        return -1;
    }

    mp->decoder = CreateVideoDecoder();
    if (!mp->decoder) {
        log_error("CreateVideoDecoder err");
        mp_cleanup_internal(mp);
        return -1;
    }

    VConfig vConfig;
    VideoStreamInfo vInfo;
    memset(&vConfig, 0, sizeof(VConfig));
    memset(&vInfo, 0, sizeof(VideoStreamInfo));

    struct CdxProgramS *program =
        &mp->media_info.program[mp->media_info.programIndex];

    if (program->audioNum > 0) {
        if (program->audioIndex < 0 || program->audioIndex >= program->audioNum) {
            program->audioIndex = 0;
        }
        mp->has_audio = 1;
        mp->audio_sample_rate = program->audio[program->audioIndex].nSampleRate;
        mp->audio_channels = program->audio[program->audioIndex].nChannelNum;
    } else {
        mp->has_audio = 0;
    }

    /* only use first video stream */
    vInfo.eCodecFormat          = program->video[0].eCodecFormat;
    vInfo.nWidth                = program->video[0].nWidth;
    vInfo.nHeight               = program->video[0].nHeight;
    vInfo.nFrameRate            = program->video[0].nFrameRate;
    vInfo.nFrameDuration        = program->video[0].nFrameDuration;
    vInfo.nAspectRatio          = program->video[0].nAspectRatio;
    vInfo.bIs3DStream           = program->video[0].bIs3DStream;
    vInfo.nCodecSpecificDataLen = program->video[0].nCodecSpecificDataLen;
    vInfo.pCodecSpecificData    = program->video[0].pCodecSpecificData;

    mp->framerate = vInfo.nFrameRate;

    int layer_width = (vInfo.nWidth + 31) & ~31;
    int layer_height = (vInfo.nHeight + 31) & ~31;

    if (drm_warpper_init_layer(mp->drm_warpper, DRM_WARPPER_LAYER_VIDEO,
                               layer_width, layer_height,
                               DRM_WARPPER_LAYER_MODE_MB32_NV12) != 0) {
        log_error("drm video layer init failed: %dx%d", layer_width, layer_height);
        mp_cleanup_internal(mp);
        return -1;
    }

    if (drm_warpper_allocate_buffer(mp->drm_warpper, DRM_WARPPER_LAYER_VIDEO,
                                    &g_video_buf) != 0) {
        log_error("drm video buffer alloc failed");
        mp_cleanup_internal(mp);
        return -1;
    }
    fill_nv12_buffer_with_color(g_video_buf.vaddr, layer_width, layer_height, 0x000000);
    drm_warpper_mount_layer(mp->drm_warpper, DRM_WARPPER_LAYER_VIDEO, 0, 0, &g_video_buf);
    g_video_buf_ready = 1;

    vConfig.eOutputPixelFormat  = PIXEL_FORMAT_YUV_MB32_420;
    vConfig.nDeInterlaceHoldingFrameBufferNum = BUF_CNT_4_DI;
    vConfig.nDisplayHoldingFrameBufferNum = BUF_CNT_4_LIST;
    vConfig.nRotateHoldingFrameBufferNum = BUF_CNT_4_ROTATE;
    vConfig.nDecodeSmoothFrameBufferNum = BUF_CNT_4_SMOOTH;
    vConfig.memops = mp->memops;
    vConfig.nVbvBufferSize = VBVBUFFERSIZE;

    ret = InitializeVideoDecoder(mp->decoder, &vInfo, &vConfig);
    if (ret != 0) {
        log_error("InitializeVideoDecoder err");
        mp_cleanup_internal(mp);
        return -1;
    }

    /* reset thread flags */
    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.end_of_stream = 0;
    mp->thread.state = 0;
    mp->thread.requested_stop = 0;
    pthread_rwlock_unlock(&mp->thread.rwlock);
    audio_queue_reset(&mp->audio_queue);
    mp->video_base_pts = 0;
    mp->video_base_time = 0;
    mp->video_last_pts = -1;
    mp->video_clock_inited = 0;

    atomic_store(&mp->running, 1);

    if (pthread_create(&mp->parser_thread, NULL, mp_parser_thread, mp) != 0) {
        log_error("parser create err");
        atomic_store(&mp->running, 0);
        mp_cleanup_internal(mp);
        return -1;
    }

    if (pthread_create(&mp->decoder_thread, NULL, mp_decoder_thread, mp) != 0) {
        log_error("decoder create err");
        pthread_rwlock_wrlock(&mp->thread.rwlock);
        mp->thread.requested_stop = 1;
        pthread_rwlock_unlock(&mp->thread.rwlock);
        audio_queue_set_eof(&mp->audio_queue);
        pthread_join(mp->parser_thread, NULL);
        atomic_store(&mp->running, 0);
        mp_cleanup_internal(mp);
        return -1;
    }

    if (mp->has_audio &&
        pthread_create(&mp->audio_thread, NULL, mp_audio_thread, mp) != 0) {
        log_error("audio create err");
        pthread_rwlock_wrlock(&mp->thread.rwlock);
        mp->thread.requested_stop = 1;
        pthread_rwlock_unlock(&mp->thread.rwlock);
        audio_queue_set_eof(&mp->audio_queue);
        pthread_join(mp->parser_thread, NULL);
        pthread_join(mp->decoder_thread, NULL);
        atomic_store(&mp->running, 0);
        mp_cleanup_internal(mp);
        return -1;
    }

    log_info("playback started");
    return 0;
}

int mediaplayer_start_raw_h264(mediaplayer_t *mp, const char *path,
                               int width, int height, int fps)
{
    if (!mp || !path || width <= 0 || height <= 0) {
        log_error("invalid raw h264 params");
        return -1;
    }

    if (atomic_load(&mp->running)) {
        log_warn("mediaplayer already running");
        return 0;
    }

    memset(mp->video_path, 0, sizeof(mp->video_path));
    snprintf(mp->video_path, sizeof(mp->video_path), "%s", path);

    mp->raw_h264_mode = 1;
    mp->raw_width = width;
    mp->raw_height = height;
    mp->raw_fps = fps > 0 ? fps : 30;
    mp->framerate = mp->raw_fps;
    mp->has_audio = 0;

    mp->memops = MemAdapterGetOpsS();
    if (!mp->memops) {
        log_error("MemAdapterGetOpsS err");
        return -1;
    }
    CdcMemOpen(mp->memops);

    mp->decoder = CreateVideoDecoder();
    if (!mp->decoder) {
        log_error("CreateVideoDecoder err");
        mp_cleanup_internal(mp);
        return -1;
    }

    VConfig vConfig;
    VideoStreamInfo vInfo;
    memset(&vConfig, 0, sizeof(VConfig));
    memset(&vInfo, 0, sizeof(VideoStreamInfo));

    vInfo.eCodecFormat   = VIDEO_CODEC_FORMAT_H264;
    vInfo.nWidth         = width;
    vInfo.nHeight        = height;
    vInfo.nFrameRate     = mp->raw_fps;
    vInfo.nFrameDuration = 1000000 / mp->raw_fps;
    vInfo.nAspectRatio   = 1000;

    int layer_width = (width + 31) & ~31;
    int layer_height = (height + 31) & ~31;

    if (drm_warpper_init_layer(mp->drm_warpper, DRM_WARPPER_LAYER_VIDEO,
                               layer_width, layer_height,
                               DRM_WARPPER_LAYER_MODE_MB32_NV12) != 0) {
        log_error("drm video layer init failed: %dx%d", layer_width, layer_height);
        mp_cleanup_internal(mp);
        return -1;
    }

    if (drm_warpper_allocate_buffer(mp->drm_warpper, DRM_WARPPER_LAYER_VIDEO,
                                    &g_video_buf) != 0) {
        log_error("drm video buffer alloc failed");
        mp_cleanup_internal(mp);
        return -1;
    }
    fill_nv12_buffer_with_color(g_video_buf.vaddr, layer_width, layer_height, 0x000000);
    drm_warpper_mount_layer(mp->drm_warpper, DRM_WARPPER_LAYER_VIDEO, 0, 0, &g_video_buf);
    g_video_buf_ready = 1;

    vConfig.eOutputPixelFormat  = PIXEL_FORMAT_YUV_MB32_420;
    vConfig.nDeInterlaceHoldingFrameBufferNum = BUF_CNT_4_DI;
    vConfig.nDisplayHoldingFrameBufferNum = BUF_CNT_4_LIST;
    vConfig.nRotateHoldingFrameBufferNum = BUF_CNT_4_ROTATE;
    vConfig.nDecodeSmoothFrameBufferNum = BUF_CNT_4_SMOOTH;
    vConfig.memops = mp->memops;
    vConfig.nVbvBufferSize = VBVBUFFERSIZE;

    int ret = InitializeVideoDecoder(mp->decoder, &vInfo, &vConfig);
    if (ret != 0) {
        log_error("InitializeVideoDecoder raw h264 err");
        mp_cleanup_internal(mp);
        return -1;
    }

    mp_reset_thread_state(mp);
    atomic_store(&mp->running, 1);

    if (pthread_create(&mp->parser_thread, NULL, mp_raw_h264_thread, mp) != 0) {
        log_error("raw h264 reader create err");
        atomic_store(&mp->running, 0);
        mp_cleanup_internal(mp);
        return -1;
    }

    if (pthread_create(&mp->decoder_thread, NULL, mp_decoder_thread, mp) != 0) {
        log_error("decoder create err");
        pthread_rwlock_wrlock(&mp->thread.rwlock);
        mp->thread.requested_stop = 1;
        pthread_rwlock_unlock(&mp->thread.rwlock);
        audio_queue_set_eof(&mp->audio_queue);
        pthread_join(mp->parser_thread, NULL);
        atomic_store(&mp->running, 0);
        mp_cleanup_internal(mp);
        return -1;
    }

    log_info("raw h264 playback started: %s %dx%d@%d",
             mp->video_path, width, height, mp->raw_fps);
    return 0;
}

mp_status_t mediaplayer_get_status(mediaplayer_t *mp)
{
    if (!mp) {
        return MP_STATUS_ERROR;
    }

    if (!atomic_load(&mp->running)) {
        return MP_STATUS_STOPPED;
    }

    pthread_rwlock_rdlock(&mp->thread.rwlock);
    int state = mp->thread.state;
    pthread_rwlock_unlock(&mp->thread.rwlock);

    if (state & (MEDIAPLAYER_PARSER_ERROR | MEDIAPLAYER_DECODER_ERROR)) {
        return MP_STATUS_ERROR;
    }

    if ((state & MEDIAPLAYER_PARSER_EXIT) &&
        (state & MEDIAPLAYER_DECODER_EXIT)) {
        return MP_STATUS_STOPPED;
    }

    return MP_STATUS_PLAYING;
}

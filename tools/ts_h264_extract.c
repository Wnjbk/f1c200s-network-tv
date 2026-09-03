/*
 * Extract an H.264 elementary stream from one MPEG-TS segment.
 *
 * This deliberately handles only the pieces needed by ordinary HLS segments:
 * PAT -> PMT -> stream type 0x1b -> PES payload. It has no codec, network,
 * container, or buffering dependency and writes the Annex-B payload to stdout.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS_PACKET_SIZE 188
#define PAT_PID 0
#define H264_STREAM_TYPE 0x1b

static int write_all(const uint8_t *buf, size_t len)
{
    while (len != 0) {
        size_t written = fwrite(buf, 1, len, stdout);
        if (written == 0) {
            return ferror(stdout) ? -1 : 0;
        }
        buf += written;
        len -= written;
    }
    return 0;
}

static const uint8_t *payload(const uint8_t *packet, size_t *len)
{
    int adaptation_control = (packet[3] >> 4) & 0x3;
    size_t offset = 4;

    if ((adaptation_control & 0x1) == 0) {
        return NULL;
    }
    if (adaptation_control & 0x2) {
        if (offset >= TS_PACKET_SIZE || offset + 1 + packet[offset] > TS_PACKET_SIZE) {
            return NULL;
        }
        offset += 1 + packet[offset];
    }
    if (offset >= TS_PACKET_SIZE) {
        return NULL;
    }
    *len = TS_PACKET_SIZE - offset;
    return packet + offset;
}

static const uint8_t *psi_section(const uint8_t *data, size_t *len, int pusi)
{
    size_t pointer;
    size_t section_length;

    if (!pusi || *len < 4) {
        return NULL;
    }
    pointer = data[0];
    if (1 + pointer + 3 > *len) {
        return NULL;
    }
    data += 1 + pointer;
    *len -= 1 + pointer;
    section_length = ((data[1] & 0x0f) << 8) | data[2];
    if (section_length + 3 > *len) {
        return NULL;
    }
    *len = section_length + 3;
    return data;
}

static int parse_pat(const uint8_t *data, size_t len, uint16_t *pmt_pid)
{
    size_t end;
    size_t i;

    if (len < 12 || data[0] != 0x00) {
        return -1;
    }
    end = len - 4;
    for (i = 8; i + 4 <= end; i += 4) {
        uint16_t program = ((uint16_t)data[i] << 8) | data[i + 1];
        if (program != 0) {
            *pmt_pid = ((uint16_t)(data[i + 2] & 0x1f) << 8) | data[i + 3];
            return 0;
        }
    }
    return -1;
}

static int parse_pmt(const uint8_t *data, size_t len, uint16_t *video_pid)
{
    size_t program_info_length;
    size_t i;
    size_t end;

    if (len < 16 || data[0] != 0x02) {
        return -1;
    }
    program_info_length = ((data[10] & 0x0f) << 8) | data[11];
    i = 12 + program_info_length;
    end = len - 4;
    while (i + 5 <= end) {
        uint8_t stream_type = data[i];
        uint16_t pid = ((uint16_t)(data[i + 1] & 0x1f) << 8) | data[i + 2];
        size_t es_info_length = ((data[i + 3] & 0x0f) << 8) | data[i + 4];

        if (stream_type == H264_STREAM_TYPE) {
            *video_pid = pid;
            return 0;
        }
        i += 5 + es_info_length;
    }
    return -1;
}

static int emit_pes(const uint8_t *data, size_t len, int pusi)
{
    if (pusi) {
        size_t header_length;
        if (len < 9 || data[0] != 0 || data[1] != 0 || data[2] != 1) {
            return -1;
        }
        header_length = 9 + data[8];
        if (header_length > len) {
            return -1;
        }
        data += header_length;
        len -= header_length;
    }
    return write_all(data, len);
}

int main(int argc, char **argv)
{
    FILE *input;
    uint8_t packet[TS_PACKET_SIZE];
    uint16_t pmt_pid = UINT16_MAX;
    uint16_t video_pid = UINT16_MAX;
    int packets = 0;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s SEGMENT.ts > video.h264\n", argv[0]);
        return 2;
    }
    input = fopen(argv[1], "rb");
    if (input == NULL) {
        fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    while (fread(packet, 1, sizeof(packet), input) == sizeof(packet)) {
        uint16_t pid;
        int pusi;
        size_t len;
        const uint8_t *data;

        if (packet[0] != 0x47) {
            fprintf(stderr, "TS sync loss after %d packets\n", packets);
            fclose(input);
            return 1;
        }
        packets++;
        pid = ((uint16_t)(packet[1] & 0x1f) << 8) | packet[2];
        pusi = (packet[1] & 0x40) != 0;
        data = payload(packet, &len);
        if (data == NULL) {
            continue;
        }
        if (pid == PAT_PID) {
            const uint8_t *section = psi_section(data, &len, pusi);
            if (section != NULL) {
                (void)parse_pat(section, len, &pmt_pid);
            }
        } else if (pid == pmt_pid) {
            const uint8_t *section = psi_section(data, &len, pusi);
            if (section != NULL) {
                (void)parse_pmt(section, len, &video_pid);
            }
        } else if (pid == video_pid && emit_pes(data, len, pusi) != 0) {
            fclose(input);
            return 1;
        }
    }
    fclose(input);
    if (video_pid == UINT16_MAX) {
        fprintf(stderr, "no H.264 stream (MPEG-TS type 0x1b) found\n");
        return 1;
    }
    return 0;
}

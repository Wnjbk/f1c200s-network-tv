#include "utils/misc.h"

#include <sys/time.h>

uint64_t get_now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

void fill_nv12_buffer_with_color(uint8_t *buf, int width, int height, uint32_t rgb)
{
    uint8_t r = (rgb >> 16) & 0xff;
    uint8_t g = (rgb >> 8) & 0xff;
    uint8_t b = rgb & 0xff;
    uint8_t y = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
    uint8_t u = (uint8_t)(((-43 * r - 85 * g + 128 * b) >> 8) + 128);
    uint8_t v = (uint8_t)(((128 * r - 107 * g - 21 * b) >> 8) + 128);
    int y_size = width * height;
    int uv_size = width * height / 2;

    for (int i = 0; i < y_size; i++) {
        buf[i] = y;
    }
    for (int i = 0; i < uv_size; i += 2) {
        buf[y_size + i] = u;
        buf[y_size + i + 1] = v;
    }
}


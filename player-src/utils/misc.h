#pragma once

#include <stdint.h>

uint64_t get_now_us(void);
void fill_nv12_buffer_with_color(uint8_t *buf, int width, int height, uint32_t rgb);


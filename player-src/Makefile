CROSS_COMPILE ?= /home/wnk/LicheePi_Nano/buildroot-2018.02.11/output/host/bin/arm-buildroot-linux-gnueabi-
SYSROOT ?= /home/wnk/LicheePi_Nano/buildroot-2018.02.11/output/host/arm-buildroot-linux-gnueabi/sysroot

CC := $(CROSS_COMPILE)gcc
STRIP := $(CROSS_COMPILE)strip

CFLAGS := --sysroot=$(SYSROOT) -Os -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DHAVE_CEDARX=1
CFLAGS += -I. -I$(SYSROOT)/usr/include -I$(SYSROOT)/usr/include/drm
CFLAGS += -I/home/wnk/LicheePi_Nano/buildroot-2018.02.11/output/build/libcedarx-custom
TARGET_DIR ?= /home/wnk/LicheePi_Nano/buildroot-2018.02.11/output/target
LDFLAGS := --sysroot=$(SYSROOT) -L$(SYSROOT)/usr/lib -L$(TARGET_DIR)/usr/lib
LDLIBS := -ldrm -lMemAdapter -lvdecoder -lVE -lvideoengine -lcdc_base
LDLIBS += -lcdx_parser -lcdx_stream -lcdx_base -lcdx_common
LDLIBS += -lfdk-aac -lasound
LDLIBS += -lssl -lcrypto -lz -lpthread -lrt -ldl -lm

SRCS := main.c \
	driver/drm_warpper.c \
	render/mediaplayer.c \
	utils/spsc_queue.c \
	utils/log.c \
	utils/misc.c

OBJS := $(SRCS:.c=.o)

all: cedar_drm_player

cedar_drm_player: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

strip: cedar_drm_player
	$(STRIP) cedar_drm_player

clean:
	rm -f $(OBJS) cedar_drm_player

.PHONY: all strip clean

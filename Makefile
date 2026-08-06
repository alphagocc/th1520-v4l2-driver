# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for the TH1520 VC8000D V4L2 decoder driver.
#
# 交叉编译示例：
#   make KDIR=/path/to/th1520-kernel ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- modules
#
# 目标内核需要打开：
#   CONFIG_MEDIA_SUPPORT, CONFIG_MEDIA_PLATFORM_SUPPORT,
#   CONFIG_V4L_MEM2MEM_DRIVERS, CONFIG_VIDEO_DEV,
#   CONFIG_MEDIA_CONTROLLER, CONFIG_MEDIA_CONTROLLER_REQUEST_API（若该版本仍有此选项）,
#   CONFIG_VIDEOBUF2_DMA_CONTIG

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# 静态检查（需要目标内核树）
check:
	$(MAKE) -C $(KDIR) M=$(PWD) C=2 CF="-D__CHECK_ENDIAN__" modules

.PHONY: modules modules_install clean check

# MT6755/MT6750 CONSYS out-of-tree modules (BTIF, WMT/STP, BT/GPS char devs, wlan gen2),
# forward-port of buddyjojo/mt6572-connsys-modules.
# Build:  make                 (against the mainline build dir ../out, arm64)
#         make KDIR=<O= dir>   (other prepared tree; cfg80211 must be =m or =y in it)
KDIR ?= $(CURDIR)/../out
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
CONSYS_PLAT ?= mt6755

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) CONSYS_PLAT=$(CONSYS_PLAT) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean

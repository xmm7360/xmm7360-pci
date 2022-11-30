obj-m := xmm7360.o

KVERSION := $(shell uname -r)
KDIR := /lib/modules/$(KVERSION)/build
PWD := $(shell pwd)
ccflags-y := -Wno-multichar

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

load:
	-sudo /sbin/rmmod iosm
	-sudo /sbin/rmmod xmm7360
	sudo /sbin/insmod xmm7360.ko

unload:
	sudo /sbin/rmmod xmm7360

reset:
	-sudo /sbin/rmmod xmm7360
	sudo dd if=/sys/bus/pci/devices/0000:3b:00.0/config of=/tmp/xmm_cfg bs=256 count=1 status=none
	sudo modprobe acpi_call
	echo '\_SB.PCI0.RP07.PXSX._RST' | sudo tee /proc/acpi/call
	sleep 1
	sudo dd of=/sys/bus/pci/devices/0000:3b:00.0/config if=/tmp/xmm_cfg bs=256 count=1 status=none

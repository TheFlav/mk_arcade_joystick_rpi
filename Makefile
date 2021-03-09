obj-m := mk_arcade_joystick_rpi.o
KVER ?= $(shell uname -r)
CPUHW ?= $(shell grep Hardware /proc/cpuinfo)


ifneq (,$(findstring -v7, $(KVER)))
 ifneq (,$(findstring BCM2711, $(CPUHW)))
  #$(info CPUHW="$(CPUHW)")
  CFLAGS_mk_arcade_joystick_rpi.o := -DRPI4
 else
  CFLAGS_mk_arcade_joystick_rpi.o := -DRPI2
 endif
endif

all:
	$(MAKE) -C /lib/modules/$(KVER)/build M=$(PWD) modules

clean:
	$(MAKE) -C /lib/modules/$(KVER)/build M=$(PWD) clean

config:
	gcc -o mk_joystick_config mk_joystick_config.cpp -lwiringPi -lpthread
	sudo ./mk_joystick_config -maxnoise 60 -adcselect
	sudo mv /opt/retropie/configs/all/emulationstation/es_input.cfg /opt/retropie/configs/all/emulationstation/es_input.cfg.bak
	@echo SYSTEM SHUTTING DOWN NOW
	sudo reboot

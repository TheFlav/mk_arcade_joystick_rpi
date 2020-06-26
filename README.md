This version of mk_arcade_joystick_rpi is intended for use with the Freeplay Zero and Freeplay CM3
DIY kits ([HERE](https://www.freeplaytech.com/product/freeplay-cm3-l2r2-adc-add-on-board/) and
[HERE](https://www.freeplaytech.com/product/freeplay-cm3-boxy-pixel-l2r2-2adc-addon/)) that
allow you to build a Raspberry Pi Zero or Raspberry Pi Compute Module 3 into a handheld portable
unit often used for retro gaming.

## Installation

### Hardware Installation

Refer to the guide
[HERE](https://docs.google.com/document/d/1GkmF4kXyXvcFUH_lFjIykzFlOVgB8iRxvx1VnZ1wgEY/).

The guide linked above gives enough information for the analog stick installation of the smaller one
that only has enough ADCs for one analog stick. For actual placement, refer to
[THIS](https://docs.google.com/document/d/13PcTNeLhDAl4OGrC_q3EINBwiPtPHSReIWtoNg8dBLU/edit) guide.

It should also be noted that installation of these can be very finicky, especially for the analog
units that use ribbon cables (some PSP sticks, PS VITA sticks, and Nintendo Switch sticks), so you
may want to look for a breakout board with ribbon connector.

### Installation Script

The `install.sh` included with this repository should build and install the driver.  Prior to running
this script, you will likely want to install/update your kernel and kernel headers, just to be sure
that they are current.

``` sh
sudo apt-get install -y --force-yes raspberrypi-kernel raspberrypi-kernel-headers
sudo shutdown -h now
# After turning back on and moving to directory containing install.sh
./install.sh
```

After you have rebooted the system, we recommend running the [config utility](#mk_joystick_config)
over `ssh` and following the prompts.

### Driver Options

Please see `install.sh` (or `/etc/modprobe.d/mk_arcade_joystick.conf`) for examples.

This command will list the available parameters.

``` sh
modinfo mk_arcade_joystick_rpi
```

### Testing/Calibrating

*These are only recommended for troubleshooting, we have a utility that automates this [HERE](#mk_joystick_config)*

Use the following commands to test (or dump) joysticks inputs :
```sh
jstest /dev/input/js0 #displays scaled values after min/max applications
jscal -c /dev/input/js0
evtest #displays raw values as they come in from the I2C chip
evdev-joystick --showcal /dev/input/event0
```

Joystick min, max, fuzz, flat can be set in `/etc/modprobe.d/mk_arcade_joystick.conf` or you can use
variations of the following commands to configure joysticks inputs on the fly:

``` sh
# The minimum and maximum values can be taken from the values evdev outputs
evdev-joystick --evdev /dev/input/event0 --axis 0 --minimum 374 --maximum 3418 --deadzone 384 --fuzz 16
evdev-joystick --evdev /dev/input/event0 --axis 1 --minimum 517 --maximum 3378 --deadzone 384 --fuzz 16
```

An additional tool, `evTestValues.sh`, is included to help find minimums and maximums. This does not
account for axis inversion, so you will need to determine that yourself.

# mk_joystick_config

This utility makes creation of a new keymap easier, accounting for analog and GPIO inputs. Detects
available I2C chips, min/max values of the axis, and if the axis is/are reversed.

After a new config file has been created, the user will need to rebind the controls for emulationstation.

To make sure this is done, we backup your old config and remove the original, which is found in
`/opt/retropie/configs/all/emulationstation/es_input.cfg.bak` should you need it. The new one can be
found in the same directory without the `.bak` extension.

*NOTE: Some users don't use the Power Slider as Hotkey, instead opting to use Select. You still
need to map the Power Slider in mk_joystick_config for correct binding behavior.*

### Usage
- **Recommended**
  - `make config`
    - runs mk_joystick_config with recommended settings and backs up your
      old emulationstation bindings, shuts down after use. You will be prompted to rebind keys in
      emulationstation on next system startup
- **Manual**
  - `sudo ./mk_joystick_config [-debug] [-maxnoise [60]] [-adcselect]`
    - If you need to debug or change the noise value
    - After this, you may want to `sudo mv /opt/retropie/configs/all/emulationstation/es_input.cfg /opt/retropie/configs/all/emulationstation/es_input.cfg.bak`

#### Optional Flags
- `-debug`, enable debug messages, Recommended: off (Optional)
- `-adcselect`, force user to select ADC chip type, default:off (Optional)
- `-maxnoise`, maximum noise allowed for ADC chip, values under 60 can cause issues, Recommended:60 (Optional)

*NOTE: Press analogs to their extremes softly, rotating slowly once or twice. Pushing too hard
or rotating them too many times will make binding hard/impossible. If you do this by accident, you
can reduce each value in the config with the same value decreased/increased by ~5% for the
larger/smaller values, respectively.*

## Changelog
- 0.1c : Bugfix
- 0.1b : Autodetection of ADC type fully implemented
- 0.1a : Initial testing release

# mk_arcade_joystick_rpi

**The information here is mainly historic from the previous version.  The version here likely works differently.**

The Raspberry Pi GPIO Joystick Driver

Other versions of mk_arcade_joystick_rpi are fully integrated in the **recalbox** distribution : see http://www.recalbox.com

** Please see the [Recalbox mk_arcade_joystick_rpi](https://github.com/recalbox/mk_arcade_joystick_rpi/) repository for more info**

## Revisions

- UPDATE 0.1.6.1 : Added support for Force Feedback via GPIO and/or PWM(PCA9633), see "Optional Flags" for information, note: can not be compares to a real controller due to hardware limitations, to be consider as experimental.

- UPDATE 0.1.6.0 : Full integration of analog config builder, new make target (`config`) and `evTestValues.sh` as an `evtest` utility.

- UPDATE 0.1.5.13 : Added ADS1015 I2C support, updated way to handle analog values, implementation the mk_joystick_config program to allow end users to easily create new configuration files.

- UPDATE 0.1.5.12 : Added support for auto center detection, New I2C device handling (to skip devices if i2c is busy), Bug fixes

- UPDATE 0.1.5.11 : Fixed support for analog parameters (min, max, fuzz, flat)

- UPDATE 0.1.5.10 : Added support for analog parameters (min, max, fuzz, flat)

- UPDATE 0.1.5.9 : Added support for 4 extra GPIO inputs

- UPDATE 0.1.5.8 : Added some new parameters to allow inverting of analog inputs

- UPDATE 0.1.5.7 : Added MCP3021 i2c support for up to 4 analog inputs

- UPDATE 0.1.5.6 : Added 4 buttons, the ability to use GPIO32 and higher, and hkmode parameter

- UPDATE 0.1.5.5 : Modified for use with Freeplay Zero/CM3

- UPDATE 0.1.5 : Added GPIO customization

- UPDATE 0.1.4 : Compatibily with rpi2 

- UPDATE 0.1.3 : Compatibily with 3.18.3
  - The driver installation now works with 3.18.3 kernel, distributed with the last firmware.

- UPDATE 0.1.2 : Downgrade to 3.12.28+
  - As the module will not load with recent kernel and headers, we add the possibility of downgrading your firmware to a compatible version, until we find a fix.

- UPDATE 0.1.1 : RPi B+ VERSION 
  - The new Raspberry Pi B+ Revision brought us 9 more GPIOs, so we are now able to connect 2 joysticks and 12 buttons directly to GPIOs. I updated the driver in order to support the 2 joysticks on GPIO configuration.

### Credits
-  [gamecon_gpio_rpi](https://github.com/petrockblog/RetroPie-Setup/wiki/gamecon_gpio_rpi) by [marqs](https://github.com/marqs85)
-  [recalbox mk_arcade_joystick_rpi](https://github.com/recalbox/mk_arcade_joystick_rpi)
-  [RetroPie-Setup](https://github.com/petrockblog/RetroPie-Setup) by [petRockBlog](http://blog.petrockblock.com/)
-  [Low Level Programming of the Raspberry Pi in C](http://www.pieter-jan.com/node/15) by [Pieter-Jan](http://www.pieter-jan.com/)

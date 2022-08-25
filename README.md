# STinG driver for USB network adapters

This is a driver for the STinG TCP/IP stack for Atari computers running TOS. It will work with USB network adapters connected to a suitable USB host adapter on the Atari, e.g. the [Lightning VME](http://wiki.newtosworld.de/index.php?title=Lightning_VME_En)/[ST](https://wiki.newtosworld.de/index.php?title=Lightning_ST), the NetUSBee, or the Unicorn.

The following USB network adapters are supported:

- Ethernet adapters based on the Asix AX88772 chipset.
- The [PicoWifi](https://github.com/czietz/picowifi/), an open-source USB-to-Wifi adapter based on the Raspberry Pi Pico W microcontroller.

Short setup guide:
* Install STinG as usual.
* Copy the USB drivers provided with your USB host adapter (e.g., `USB.PRG` and `BLITZ*.PRG` for the Lightning VME/ST) to the `AUTO` folder of your boot drive.
* Copy `usb_net.stx` to the `STING` folder.
* Reboot and make sure to load `STING.PRG` after the USB drivers.
* Enable and configure the _USBether_ STinG device in the usual way, setting up IP address, network mask, DNS server, etc. Don't forget to edit STinG's `ROUTE.TAB`.

Please refer to the appropriate documentation for more information about setting up your respective USB host adapter and the STinG network stack.


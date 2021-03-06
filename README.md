# STinG driver for Asix USB-to-Ethernet adapters

This is a driver for the STinG TCP/IP stack for Atari computers running TOS. It will work with USB-to-Ethernet adapters based on the Asix AX88772 chipset connected to a suitable USB host adapter on the Atari, e.g. the [Lightning VME](http://wiki.newtosworld.de/index.php?title=Lightning_VME_En) or the Unicorn.

Short setup guide:
* Install STinG as usual.
* Copy the USB drivers provided with your USB host adapter (e.g. `USB.PRG` and `BLITZ*.PRG` for the Lightning VME) to the `AUTO` folder of your boot drive.
* Copy `usb_asix.stx` to the `STING` folder.
* Reboot.
* Enable and configure the _USBether_ STinG device in the usual way, setting up IP address, network mask, DNS server, etc. Don't forget to edit STinG's `ROUTE.TAB`.

__Note__: Unfortunately, the USB API that is maintained by the FreeMiNT developers is subject to occasional breaking changes. `usb_asix.stx` has been compiled for the current version 4 of this API. In case your USB drivers still use the previous version 3, please download `usb_asix.stx` from https://github.com/czietz/usbsting/tree/usb_api_v3. For the old version 1, please use https://github.com/czietz/usbsting/tree/usb_api_v1.

Please refer to the appropriate documentation for more information about setting up your respective USB host adapter and the STinG network stack.


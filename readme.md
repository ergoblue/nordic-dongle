# About This Project

This project connects to a Bluetooth peripheral (keyboard or mouse) as a central
and makes the HID data available via USB. In most cases, you will want to use
Bluetooth directly (or a regular USB Bluetooth dongle if your device has USB but
lacks Bluetooth). However this project is useful if you have a Bluetooth
peripheral in the following cases:

- You frequently need to access BIOS.
- You use full disk encryption (it is difficult to get Bluetooth working before
  the disk is decrypted).
- You have multiple operating systems on the same machine (using Bluetooth
  directly may be tricky as the different operating systems will have the
  same MAC address while having different encryption keys).

This project is not designed to be used as is. Rather, it should be a starting
point to customize for your own needs.

This has been tested with the
[Nordic nRF52840 Dongle](https://www.digikey.com/product-detail/en/nordic-semiconductor-asa/NRF52840-DONGLE/1490-1073-ND/9491124)
though there are many alternatives that also work.

# High level instructions:

- Update `report_map.h` with the HID report.
- Update `peripheral_mac` in `main.c` with the MAC address of the peripheral.
- Update `ble_gap_sec_params_t` in `main.c` with the security parameters of
  your peripheral. You may want to disable passkey authentication if the
  peripheral doesn't support it.
- Copy `examples/ble_central/ble_app_hrs_c/pca10056/s140/config/sdk_config.h` to
  `pca10059/s140/config/sdk_config.h` in this repository.
- Run `make` in `pca10059/s140/armgcc`.

# Known Issues

- This makes no attempt at parsing the HID report.
- This doesn't work for the MacOS FileVault (though I suspect that is due to
  some broader issue with the Nordic nRF52840).

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

# High Level Instructions

- Update `peripheral_mac` in `main.c` with the MAC address of the peripheral.
- Update `ble_gap_sec_params_t` in `main.c` to match the security parameters of
  your peripheral. In particular, you may want to disable passkey authentication
  and/or LESC if the peripheral doesn't support it.
- Update `report_map.h` with the HID report of the peripheral. Note that this
  project makes no attempt to parse the peripheral's HID report and will not do
  any translation/conversion. Note that you may want to do this after bonding if
  passkey authentication is necessary (otherwise you also have to update
  `passkey_output_char` accordingly).
- Copy `examples/ble_central/ble_app_hrs_c/pca10056/s140/config/sdk_config.h` to
  `pca10059/s140/config/sdk_config.h` in this repository.
- Run `make` in `pca10059/s140/armgcc` and flash compiled firmware to device.
- If using passkey authentication, the USB dongle will "display" the passcode by
  typing it.
- If necessary, press white button (P1.06) to delete bond with peripheral.

# Known Issues

- This doesn't work for the MacOS FileVault (though I suspect that is due to
  some broader issue with the Nordic nRF52840).

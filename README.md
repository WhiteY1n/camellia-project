# Secure USB Crypto

## Overview

Secure USB Crypto is a Linux kernel project that exposes a misc device at `/dev/crypto_mouse`.
The driver uses a specific USB mouse dongle as a physical key, then lets user space load a Camellia key and encrypt or decrypt buffered data through ioctl calls.

## Features

* Kernel-space Camellia encryption and decryption in `usb_crypto_drv.c`.
* `/dev/crypto_mouse` misc device with `read`, `write`, `llseek`, and ioctl support.
* USB-gated access based on VID/PID `1a81:101f`.
* Read, write, encrypt, and decrypt calls are denied when the target mouse is unplugged.
* Key loading and zeroization through ioctls.
* PKCS#7 padding validation during decryption.
* Status reporting for mouse presence, key state, buffer length, encrypted state, and plain length.
* Chunked file encryption in the CLI for files larger than the driver buffer limit.
* A Python Tkinter GUI that calls the CLI and can generate a key file from `/proc/mouse_entropy`.
* A companion mouse filter module that exposes `/proc/mouse_entropy` for key generation.

## Tech Stack

* Backend: Linux kernel module written in C, misc device, USB driver callbacks, notifier hooks, ioctl API.
* Database: None.
* Tools / integrations: Camellia kernel crypto API, Python Tkinter GUI, Python `cryptography` library, `/proc` entropy source from the companion mouse filter module.

## API Examples (if applicable)

* `ioctl(/dev/crypto_mouse, CRYPTO_MOUSE_IOC_SET_KEY)` - load a 16/24/32-byte Camellia key.
* `ioctl(/dev/crypto_mouse, CRYPTO_MOUSE_IOC_ENCRYPT)` - encrypt the current buffer with PKCS#7 padding.
* `ioctl(/dev/crypto_mouse, CRYPTO_MOUSE_IOC_GET_STATUS)` - read device state from user space.

## How to Run

* Build the driver: `make -C secure_usb_crypto/driver`
* Build the CLI: `make -C secure_usb_crypto/app`
* Load the module: `sudo insmod secure_usb_crypto/driver/usb_crypto_drv.ko`
* Confirm the device node exists: `ls -l /dev/crypto_mouse`
* Run the CLI from `secure_usb_crypto/app`.

## Notes

* The driver stores data in a fixed buffer of `CRYPTO_MOUSE_MAX_DATA` bytes.
* File encryption uses a chunked format with `CMCHUNK1`, per-chunk plaintext length, and ciphertext length headers.
* Decryption falls back to a legacy single-blob format when the chunk header is absent.
* The GUI is user-space only; the backend behavior lives in the kernel driver and CLI.



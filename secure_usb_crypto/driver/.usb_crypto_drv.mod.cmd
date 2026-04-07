savedcmd_usb_crypto_drv.mod := printf '%s\n'   usb_crypto_drv.o | awk '!x[$$0]++ { print("./"$$0) }' > usb_crypto_drv.mod

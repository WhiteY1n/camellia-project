# Secure USB Crypto (Camellia Kernel Driver)

Du an minh hoa co che "USB mouse = physical key" de quan ly file bao mat tren Ubuntu 64-bit.

## 1) Doi chieu voi de bai

### Yeu cau 1: Quan ly file bao mat, ma hoa bang CAMELLIA trong driver
- Da dap ung.
- Ma hoa/giai ma duoc thuc hien trong kernel module `usb_crypto_drv.ko`.
- App user space chi gui/nhan du lieu va goi ioctl.
- Da ho tro PKCS#7 padding de ma hoa du lieu co do dai bat ky.

### Yeu cau 2: Ubuntu 64-bit, minh hoa voi chuot USB
- Da dap ung.
- Driver khoa/mo truy cap dua tren trang thai chuot USB dung VID/PID: `1a81:101f` (Holtek wireless dongle).
- Rut chuot: thao tac read/write/encrypt/decrypt bi tu choi.
- Cam chuot: cho phep thao tac.

## 2) Kien truc nhanh
- Kernel module: `driver/usb_crypto_drv.c`
- Shared ioctl header: `include/crypto_mouse_ioctl.h`
- User app CLI: `app/crypto_mouse_cli.c`
- Device node: `/dev/crypto_mouse`

## 3) Build

### Build driver
```bash
cd /home/chuvu/camellia-project/secure_usb_crypto/driver
make
```

### Build app
```bash
cd /home/chuvu/camellia-project/secure_usb_crypto/app
make
```

## 4) Nap/GO module

### Nap module
```bash
cd /home/chuvu/camellia-project/secure_usb_crypto/driver
sudo insmod ./usb_crypto_drv.ko
```

### Kiem tra device
```bash
ls -l /dev/crypto_mouse
```

### Go module
```bash
sudo rmmod usb_crypto_drv
```

## 5) Lenh CLI

```bash
cd /home/chuvu/camellia-project/secure_usb_crypto/app
./crypto_mouse_cli status
./crypto_mouse_cli setkey 00112233445566778899aabbccddeeff
./crypto_mouse_cli clearkey
./crypto_mouse_cli write HELLO_CAMELLIA
./crypto_mouse_cli read
./crypto_mouse_cli encrypt
./crypto_mouse_cli decrypt
```

### Ma hoa/giai ma file
```bash
./crypto_mouse_cli encrypt-file 00112233445566778899aabbccddeeff input.txt input.enc
./crypto_mouse_cli decrypt-file 00112233445566778899aabbccddeeff input.enc output.txt
```

## 6) Demo de tai (goi y thuyet trinh)

### Buoc A: Chuan bi
1. Cam chuot USB dongle `1a81:101f`.
2. Build driver + app.
3. Nap module.

### Buoc B: Demo mo khoa bang physical key
1. Khi chuot dang cam:
```bash
echo ok_when_plugged > /dev/crypto_mouse
```
2. Rut chuot.
3. Thu lai:
```bash
echo blocked_when_unplugged > /dev/crypto_mouse
```
Ky vong: `Permission denied`.

### Buoc C: Demo ma hoa file
1. Tao file ro:
```bash
echo "camellia kernel demo" > demo.txt
```
2. Ma hoa:
```bash
./crypto_mouse_cli encrypt-file 00112233445566778899aabbccddeeff demo.txt demo.enc
```
3. Giai ma:
```bash
./crypto_mouse_cli decrypt-file 00112233445566778899aabbccddeeff demo.enc demo.dec
```
4. So sanh:
```bash
cmp -s demo.txt demo.dec && echo MATCH || echo MISMATCH
```
Ky vong: `MATCH`.

## 7) Log de bao cao
```bash
sudo dmesg | grep -Ei "crypto_mouse|usb_crypto_drv|notifier|encrypt|decrypt|denied" | tail -n 100
```

## 8) Luu y ky thuat
- Ban hien tai gioi han du lieu 1 lan xu ly toi da `4096` bytes.
- Che do ma hoa dang o muc demo hoc tap (ECB + PKCS#7), phu hop minh hoa de tai.
- Neu can ban nang cao de bao cao an toan hon, co the nang cap CBC/XTS + IV + dinh dang file co header.

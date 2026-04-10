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

## 2.1) Ban do luong chay ngan

Luong tong quat:
- User (CLI/GUI) -> /dev/crypto_mouse -> ioctl/read/write -> Driver (Camellia) -> ket qua tra ve app.
- Dieu kien mo khoa: USB mouse dung VID/PID dang cam.
- Rut mouse: driver tra loi EACCES va zeroize key/data.

Luong encrypt-file:
- input file -> CLI cat chunk (toi da MAX_DATA - 16)
- moi chunk: write(device) -> ioctl(ENCRYPT) -> GET_STATUS lay data_len -> read(device)
- CLI ghi ra output theo stream: [CMCHUNK1][plain_len][cipher_len][cipher_bytes]...

Luong decrypt-file:
- CLI doc magic dau file:
- Neu la CMCHUNK1: lap theo chunk -> write(device) -> ioctl(DECRYPT) -> GET_STATUS -> read(device) -> ghi plain ra file.
- Neu khong phai CMCHUNK1: fallback ve che do legacy (1 blob <= MAX_DATA).
- Neu padding/du lieu sai: driver tra loi loi (thuong EBADMSG/EINVAL), CLI dung file loi do.

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
```
cd /home/chuvu/camellia-project/secure_usb_crypto
make -C driver
make -C app
```
3. Nap module.
```
sudo rmmod usb_crypto_drv 2>/dev/null || true
sudo insmod /home/chuvu/camellia-project/secure_usb_crypto/driver/usb_crypto_drv.ko
ls -l /dev/crypto_mouse
```
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
- Driver van xu ly tung block toi da `262144` bytes/lan, nhung CLI da chia chunk khi encrypt/decrypt file nen co the xu ly file lon hon gioi han nay.
- Che do ma hoa dang o muc demo hoc tap (ECB + PKCS#7), phu hop minh hoa de tai.
- Neu can ban nang cao de bao cao an toan hon, co the nang cap CBC/XTS + IV + dinh dang file co header.

## 9) Don dep sau khi demo

### Buoc 1: Go module khoi kernel
```bash
sudo rmmod usb_crypto_drv 2>/dev/null || true
```

### Buoc 2: Xoa file demo tam trong app
```bash
cd /home/chuvu/camellia-project/secure_usb_crypto
rm -f app/demo.txt app/demo.enc app/demo.dec
rm -f app/sample_input.txt app/sample.enc app/sample.dec
```

### Buoc 3: Don dep build artifacts
```bash
make -C driver clean
make -C app clean
```

## 10) GUI (Tkinter) cho encrypt/decrypt

GUI file nam tai: `app/crypto_mouse_gui.py`

### Cai dependency GUI
```bash
cd /home/chuvu/camellia-project/secure_usb_crypto
sudo apt-get update
sudo apt-get install -y python3 python3-tk python3-cryptography
```

### Build app CLI (GUI se goi lai CLI)
```bash
cd /home/chuvu/camellia-project/secure_usb_crypto
make -C app
```

### Chay GUI
Luu y: de lay entropy tu driver `mouse_input_filter` qua `/proc/mouse_entropy`, nen chay bang sudo va can nap module `mouse_input_filter` truoc.

```bash
cd /home/chuvu/camellia-project/secure_usb_crypto
sudo python3 app/crypto_mouse_gui.py
```

Neu chua nap `mouse_input_filter`:
```bash
cd /home/chuvu/camellia-project/mouse_driver_module
make
sudo insmod mouse_input_filter.ko
```

### Tinh nang GUI moi
- Progress bar realtime khi encrypt/decrypt batch va folder.
- Nut `Open Last Output Folder` de mo nhanh thu muc chua file ket qua.
- GUI tu dong cap nhat trang thai USB mouse (cam/rut) theo thoi gian thuc, khong can bam `Check Status`.
- Co them tab `Modify` de sua noi dung file text `.enc` va luu de file `.enc` cu.

### Luong Encrypt trong GUI
1. Chon scope: File(s) / Folder recursive.
2. Bam `Select Target` de chon file/folder.
3. Bam `Generate Key From USB Mouse`.
4. Di chuyen chuot trong 5 giay.
5. Nhap passphrase va luu key file (vi du `secret.key`).
6. Bam `Encrypt`.

Ket qua:
- File duoc tao them duoi `.enc`.
- File da co duoi `.enc` se duoc bo qua trong batch/folder.
- Neu scope la `Folder recursive`: GUI se nen ca thu muc thanh 1 file `.zip`, sau do ma hoa thanh `ten_thu_muc.zip.enc`.

### Luong Decrypt trong GUI
1. Chon scope va chon target `.enc`.
2. Bam `Decrypt (Using Key File)`.
3. Chon `secret.key` + nhap passphrase.
4. GUI giai ma va tao file output:
	- Neu input la `abc.txt.enc` -> output `abc.txt`
	- Neu input khong dung duoi `.enc` -> output them `.dec`
	- Neu input la `ten_thu_muc.zip.enc` -> output `ten_thu_muc.zip`, sau do GUI tu dong giai nen ra thu muc.

### Xu ly loi quan trong
- Neu rut chuot USB key: driver se chan read/write/encrypt/decrypt.
- Neu file `.enc` bi sua/hong: decrypt file do se bao loi va dung file do.
- Trong batch/folder: file loi se duoc ghi vao tong ket, cac file khac van tiep tuc.
- Encrypt/decrypt file trong GUI da su dung co che chunk cua CLI, nen khong con bi chan boi gioi han tong kich thuoc file.

### Tab Modify (chi ho tro text)
1. Chon file `.enc` text can sua.
2. Bam `Load For Edit`, nhap key file + passphrase de decrypt vao editor.
3. Sua noi dung text trong editor.
4. Bam `Save (Overwrite .enc)` de ma hoa lai va ghi de file `.enc` cu.

Gioi han hien tai:
- Chi ho tro file text UTF-8.
- Khong backup.
- Chua ho tro modify file `.zip.enc`.



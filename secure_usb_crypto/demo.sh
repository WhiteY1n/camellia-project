#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
DRV_DIR="$ROOT_DIR/driver"
APP_DIR="$ROOT_DIR/app"
KEY_HEX="00112233445566778899aabbccddeeff"

echo "[1/6] Build driver"
make -C "$DRV_DIR"

echo "[2/6] Build app"
make -C "$APP_DIR"

echo "[3/6] Reload module"
sudo rmmod usb_crypto_drv 2>/dev/null || true
sudo insmod "$DRV_DIR/usb_crypto_drv.ko"

if [[ ! -e /dev/crypto_mouse ]]; then
  echo "[ERR] /dev/crypto_mouse khong ton tai"
  exit 1
fi

echo "[4/6] Tao file mau"
cd "$APP_DIR"
echo "camellia kernel demo" > demo.txt

echo "[5/6] Encrypt/decrypt file"
./crypto_mouse_cli encrypt-file "$KEY_HEX" demo.txt demo.enc
./crypto_mouse_cli decrypt-file "$KEY_HEX" demo.enc demo.dec

echo "[6/6] Verify"
if cmp -s demo.txt demo.dec; then
  echo "DEMO_OK: file giai ma trung file goc"
else
  echo "DEMO_FAIL: file giai ma khong trung file goc"
  exit 1
fi

echo
echo "Goi y demo USB key lock:"
echo "  1) Rut chuot va chay: echo blocked > /dev/crypto_mouse"
echo "  2) Cam lai chuot va chay: echo allowed > /dev/crypto_mouse"
echo
echo "Xem log: sudo dmesg | grep -Ei 'crypto_mouse|usb_crypto_drv|notifier|denied' | tail -n 80"

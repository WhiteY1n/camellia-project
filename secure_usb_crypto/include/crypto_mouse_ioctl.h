#ifndef CRYPTO_MOUSE_IOCTL_H
#define CRYPTO_MOUSE_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Magic chung de dong bo nhom ioctl giua user-space va driver. */
#define CRYPTO_MOUSE_IOC_MAGIC 'k'

/* Gioi han bo nho du lieu va do dai key Camellia. */
#define CRYPTO_MOUSE_MAX_DATA (256 * 1024)
#define CRYPTO_MOUSE_MAX_KEY 32

/* Payload cho ioctl SET_KEY. */
struct crypto_mouse_key {
    __u32 key_len;
    __u8 key[CRYPTO_MOUSE_MAX_KEY];
};

/* Snapshot trang thai driver cho ioctl GET_STATUS. */
struct crypto_mouse_status {
    __u32 mouse_present;
    __u32 key_ready;
    __u32 data_len;
    __u32 is_encrypted;
    __u32 plain_len;
};

/* Nhom lenh dieu khien driver. */
#define CRYPTO_MOUSE_IOC_SET_KEY _IOW(CRYPTO_MOUSE_IOC_MAGIC, 1, struct crypto_mouse_key)
#define CRYPTO_MOUSE_IOC_ENCRYPT _IO(CRYPTO_MOUSE_IOC_MAGIC, 2)
#define CRYPTO_MOUSE_IOC_DECRYPT _IO(CRYPTO_MOUSE_IOC_MAGIC, 3)
#define CRYPTO_MOUSE_IOC_GET_STATUS _IOR(CRYPTO_MOUSE_IOC_MAGIC, 4, struct crypto_mouse_status)
#define CRYPTO_MOUSE_IOC_CLEAR_KEY _IO(CRYPTO_MOUSE_IOC_MAGIC, 5)

#endif

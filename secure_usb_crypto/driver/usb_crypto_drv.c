#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/notifier.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <crypto/internal/cipher.h>

#include "crypto_mouse_ioctl.h"

/*
 * Tong quan driver:
 * - Tao thiet bi char /dev/crypto_mouse de user-space doc/ghi + goi ioctl.
 * - Chi cho phep thao tac khi USB mouse dung VID/PID dang cam.
 * - Du lieu va key nam trong bo nho kernel, duoc bao ve bang mutex.
 * - Ma hoa/giai ma dung Camellia theo block 16 bytes + PKCS#7 padding.
 */

static int is_mouse_plugged = 0;
static DEFINE_MUTEX(crypto_lock);
/* Buffer du lieu dang xu ly trong driver (plain hoac cipher). */
static u8 crypto_data[CRYPTO_MOUSE_MAX_DATA];
/* Do dai du lieu hien tai trong crypto_data. */
static size_t crypto_data_len;
/* Do dai plaintext truoc khi padding. */
static size_t crypto_plain_len;
/* Co danh dau buffer hien tai dang o trang thai da ma hoa hay khong. */
static bool crypto_is_encrypted;
/* Key Camellia duoc nap tu user-space qua ioctl SET_KEY. */
static u8 camellia_key[CRYPTO_MOUSE_MAX_KEY];
static u32 camellia_key_len;

static void crypto_mouse_zeroize_data_locked(void)
{
    /* Xoa sach du lieu nhay cam khi rut chuot/thoat module. */
    memzero_explicit(crypto_data, sizeof(crypto_data));
    crypto_data_len = 0;
    crypto_plain_len = 0;
    crypto_is_encrypted = false;
}

static void crypto_mouse_zeroize_key_locked(void)
{
    memzero_explicit(camellia_key, sizeof(camellia_key));
    camellia_key_len = 0;
}

static void crypto_mouse_lock_on_unplug(void)
{
    /* Coi viec rut chuot nhu mat "physical key" -> khoa toan bo state. */
    mutex_lock(&crypto_lock);
    crypto_mouse_zeroize_data_locked();
    crypto_mouse_zeroize_key_locked();
    mutex_unlock(&crypto_lock);
}

static bool usb_crypto_is_target_mouse(struct usb_device *udev)
{
    /* VID/PID cua USB dongle chuot duoc chon lam key vat ly. */
    return le16_to_cpu(udev->descriptor.idVendor) == 0x1a81 &&
           le16_to_cpu(udev->descriptor.idProduct) == 0x101f;
}

static int usb_crypto_scan_cb(struct usb_device *udev, void *data)
{
    int *found = data;

    if (usb_crypto_is_target_mouse(udev)) {
        *found = 1;
        return 1;
    }

    return 0;
}

static void usb_crypto_detect_mouse_present(void)
{
    int found = 0;

    /* Quet trang thai hien tai luc module vua load. */
    usb_for_each_dev(&found, usb_crypto_scan_cb);
    is_mouse_plugged = found;

    if (found)
        printk(KERN_INFO "crypto_mouse: target mouse already present\n");
    else
        printk(KERN_INFO "crypto_mouse: target mouse not present\n");
}

static int usb_crypto_notify(struct notifier_block *self,
                             unsigned long action, void *data)
{
    struct usb_device *udev = data;

    if (!udev || !usb_crypto_is_target_mouse(udev))
        return NOTIFY_DONE;

    if (action == USB_DEVICE_ADD) {
        /* Cam chuot vao -> mo quyen thao tac cho /dev/crypto_mouse. */
        is_mouse_plugged = 1;
        printk(KERN_INFO
               "crypto_mouse: notifier add (VID=0x%04x, PID=0x%04x)\n",
               le16_to_cpu(udev->descriptor.idVendor),
               le16_to_cpu(udev->descriptor.idProduct));
        return NOTIFY_OK;
    }

    if (action == USB_DEVICE_REMOVE) {
        /* Rut chuot -> dong vai tro key vat ly va xoa state nhay cam. */
        is_mouse_plugged = 0;
        crypto_mouse_lock_on_unplug();
        printk(KERN_INFO "crypto_mouse: notifier remove\n");
        return NOTIFY_OK;
    }

    return NOTIFY_DONE;
}

static struct notifier_block usb_crypto_nb = {
    .notifier_call = usb_crypto_notify,
};

static int crypto_mouse_open(struct inode *inode, struct file *file)
{
    (void)inode;
    (void)file;

    printk(KERN_INFO "crypto_mouse: device opened\n");
    return 0;
}

static int crypto_mouse_release(struct inode *inode, struct file *file)
{
    (void)inode;
    (void)file;

    printk(KERN_INFO "crypto_mouse: device released\n");
    return 0;
}

static ssize_t crypto_mouse_read(struct file *file, char __user *buf,
                                 size_t count, loff_t *ppos)
{
    size_t to_copy;

    (void)file;

    if (!is_mouse_plugged) {
        printk(KERN_WARNING "crypto_mouse: read denied (mouse key not present)\n");
        return -EACCES;
    }

    mutex_lock(&crypto_lock);

    /* Read ho tro theo offset (*ppos), nen user-space co the doc nhieu lan. */
    if (*ppos >= crypto_data_len) {
        mutex_unlock(&crypto_lock);
        return 0;
    }

    to_copy = min(count, crypto_data_len - (size_t)*ppos);
    if (copy_to_user(buf, crypto_data + *ppos, to_copy)) {
        mutex_unlock(&crypto_lock);
        return -EFAULT;
    }

    *ppos += to_copy;
    mutex_unlock(&crypto_lock);

    printk(KERN_INFO "crypto_mouse: read %zu bytes\n", to_copy);
    return to_copy;
}

static ssize_t crypto_mouse_write(struct file *file, const char __user *buf,
                                  size_t count, loff_t *ppos)
{
    (void)file;
    (void)ppos;

    if (!is_mouse_plugged) {
        printk(KERN_WARNING "crypto_mouse: write denied (mouse key not present)\n");
        return -EACCES;
    }

    if (count > CRYPTO_MOUSE_MAX_DATA)
        return -EMSGSIZE;

    mutex_lock(&crypto_lock);
    if (copy_from_user(crypto_data, buf, count)) {
        mutex_unlock(&crypto_lock);
        return -EFAULT;
    }

    /* Moi lan write duoc xem la plaintext moi, reset trang thai cipher. */
    crypto_data_len = count;
    crypto_plain_len = count;
    crypto_is_encrypted = false;
    mutex_unlock(&crypto_lock);

    printk(KERN_INFO "crypto_mouse: write received, len=%zu\n", count);
    return count;
}

static int crypto_mouse_transform_buffer(bool encrypt)
{
    struct crypto_cipher *tfm;
    size_t i;
    int ret;

    if (!camellia_key_len)
        return -ENOKEY;

    if (!crypto_data_len)
        return -ENODATA;

    if (crypto_data_len % 16 != 0)
        return -EINVAL;

    tfm = crypto_alloc_cipher("camellia", 0, 0);
    if (IS_ERR(tfm))
        return PTR_ERR(tfm);

    ret = crypto_cipher_setkey(tfm, camellia_key, camellia_key_len);
    if (ret) {
        crypto_free_cipher(tfm);
        return ret;
    }

    /*
     * Xu ly theo tung block 16 bytes in-place.
     * Dang nay la block cipher truc tiep tren buffer (tuong duong ECB demo).
     */
    for (i = 0; i < crypto_data_len; i += 16) {
        if (encrypt)
            crypto_cipher_encrypt_one(tfm, crypto_data + i, crypto_data + i);
        else
            crypto_cipher_decrypt_one(tfm, crypto_data + i, crypto_data + i);
    }

    crypto_free_cipher(tfm);
    return 0;
}

static int crypto_mouse_encrypt_pkcs7_locked(void)
{
    size_t pad_len;

    if (crypto_is_encrypted)
        return -EALREADY;

    if (!crypto_plain_len)
        return -ENODATA;

    /* PKCS#7: luon them it nhat 1 block padding. */
    pad_len = 16 - (crypto_plain_len % 16);
    if (pad_len == 0)
        pad_len = 16;

    if (crypto_plain_len + pad_len > CRYPTO_MOUSE_MAX_DATA)
        return -EMSGSIZE;

    memset(crypto_data + crypto_plain_len, (u8)pad_len, pad_len);
    crypto_data_len = crypto_plain_len + pad_len;

    return crypto_mouse_transform_buffer(true);
}

static int crypto_mouse_decrypt_pkcs7_locked(void)
{
    u8 pad_len;
    size_t i;
    int ret;

    if (crypto_data_len == 0 || (crypto_data_len % 16) != 0)
        return -EINVAL;

    ret = crypto_mouse_transform_buffer(false);
    if (ret)
        return ret;

    if (!crypto_data_len)
        return -EBADMSG;

    /* Kiem tra padding PKCS#7 de phat hien du lieu/keys sai. */
    pad_len = crypto_data[crypto_data_len - 1];
    if (pad_len == 0 || pad_len > 16 || pad_len > crypto_data_len)
        return -EBADMSG;

    for (i = 0; i < pad_len; i++) {
        if (crypto_data[crypto_data_len - 1 - i] != pad_len)
            return -EBADMSG;
    }

    crypto_plain_len = crypto_data_len - pad_len;
    crypto_data_len = crypto_plain_len;
    return 0;
}

static long crypto_mouse_ioctl(struct file *file, unsigned int cmd,
                               unsigned long arg)
{
    struct crypto_mouse_key req_key;
    struct crypto_mouse_status st;
    int ret = 0;

    (void)file;

    switch (cmd) {
    case CRYPTO_MOUSE_IOC_SET_KEY:
        /* User-space gui key Camellia vao driver. */
        if (!is_mouse_plugged)
            return -EACCES;

        if (copy_from_user(&req_key, (void __user *)arg, sizeof(req_key)))
            return -EFAULT;

        if (req_key.key_len != 16 && req_key.key_len != 24 &&
            req_key.key_len != 32)
            return -EINVAL;

        mutex_lock(&crypto_lock);
        crypto_mouse_zeroize_key_locked();
        memcpy(camellia_key, req_key.key, req_key.key_len);
        camellia_key_len = req_key.key_len;
        mutex_unlock(&crypto_lock);

        printk(KERN_INFO "crypto_mouse: key set (len=%u)\n", req_key.key_len);
        return 0;

    case CRYPTO_MOUSE_IOC_ENCRYPT:
        /* Encrypt tren du lieu hien dang nam trong crypto_data. */
        if (!is_mouse_plugged)
            return -EACCES;

        mutex_lock(&crypto_lock);
        ret = crypto_mouse_encrypt_pkcs7_locked();
        if (!ret)
            crypto_is_encrypted = true;
        mutex_unlock(&crypto_lock);
        if (!ret)
            printk(KERN_INFO "crypto_mouse: camellia encrypt done\n");
        return ret;

    case CRYPTO_MOUSE_IOC_DECRYPT:
        /* Decrypt va validate PKCS#7; fail neu ciphertext hu/sai key. */
        if (!is_mouse_plugged)
            return -EACCES;

        mutex_lock(&crypto_lock);
        ret = crypto_mouse_decrypt_pkcs7_locked();
        if (!ret)
            crypto_is_encrypted = false;
        mutex_unlock(&crypto_lock);
        if (!ret)
            printk(KERN_INFO "crypto_mouse: camellia decrypt done\n");
        return ret;

    case CRYPTO_MOUSE_IOC_GET_STATUS:
        /* GUI/CLI doc status de biet mouse/key/data dang o trang thai nao. */
        mutex_lock(&crypto_lock);
        st.mouse_present = is_mouse_plugged ? 1 : 0;
        st.key_ready = camellia_key_len ? 1 : 0;
        st.data_len = crypto_data_len;
        st.is_encrypted = crypto_is_encrypted ? 1 : 0;
        st.plain_len = crypto_plain_len;
        mutex_unlock(&crypto_lock);

        if (copy_to_user((void __user *)arg, &st, sizeof(st)))
            return -EFAULT;

        return 0;

    case CRYPTO_MOUSE_IOC_CLEAR_KEY:
        /* Xoa key chu dong theo lenh user-space. */
        mutex_lock(&crypto_lock);
        crypto_mouse_zeroize_key_locked();
        mutex_unlock(&crypto_lock);
        printk(KERN_INFO "crypto_mouse: key cleared\n");
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations crypto_mouse_fops = {
    .owner = THIS_MODULE,
    .open = crypto_mouse_open,
    .release = crypto_mouse_release,
    .read = crypto_mouse_read,
    .write = crypto_mouse_write,
    .unlocked_ioctl = crypto_mouse_ioctl,
};

static struct miscdevice crypto_mouse_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "crypto_mouse",
    .fops = &crypto_mouse_fops,
    .mode = 0666,
};

static int usb_crypto_probe(struct usb_interface *interface,
                            const struct usb_device_id *id)
{
    (void)interface;

    is_mouse_plugged = 1;
    printk(KERN_INFO "crypto_mouse: mouse plugged (VID=0x%04x, PID=0x%04x)\n",
           id->idVendor, id->idProduct);
    return 0;
}

static void usb_crypto_disconnect(struct usb_interface *interface)
{
    (void)interface;

    is_mouse_plugged = 0;
    printk(KERN_INFO "crypto_mouse: mouse unplugged\n");
}

static const struct usb_device_id usb_crypto_id_table[] = {
    { USB_DEVICE(0x1a81, 0x101f) },
    { }
};
MODULE_DEVICE_TABLE(usb, usb_crypto_id_table);

static struct usb_driver usb_crypto_driver = {
    .name = "usb_crypto_drv",
    .probe = usb_crypto_probe,
    .disconnect = usb_crypto_disconnect,
    .id_table = usb_crypto_id_table,
};

static int __init usb_crypto_drv_init(void)
{
    int ret;

    /* Khoi tao state mac dinh khi module vua nap. */
    crypto_data_len = 0;
    crypto_plain_len = 0;
    crypto_is_encrypted = false;
    camellia_key_len = 0;

    /* Dang ky notifier de theo doi cam/rut USB theo thoi gian thuc. */
    usb_register_notify(&usb_crypto_nb);
    usb_crypto_detect_mouse_present();

    ret = usb_register(&usb_crypto_driver);
    if (ret) {
        printk(KERN_ERR "crypto_mouse: usb_register failed (%d)\n", ret);
        usb_unregister_notify(&usb_crypto_nb);
        return ret;
    }

    ret = misc_register(&crypto_mouse_miscdev);
    if (ret) {
        printk(KERN_ERR "crypto_mouse: misc_register failed (%d)\n", ret);
        usb_deregister(&usb_crypto_driver);
        usb_unregister_notify(&usb_crypto_nb);
        return ret;
    }

    printk(KERN_INFO "Module loaded\n");
    return 0;
}

static void __exit usb_crypto_drv_exit(void)
{
    /* Thu hoi tai nguyen + xoa du lieu truoc khi roi kernel. */
    crypto_mouse_lock_on_unplug();
    misc_deregister(&crypto_mouse_miscdev);
    usb_deregister(&usb_crypto_driver);
    usb_unregister_notify(&usb_crypto_nb);
    printk(KERN_INFO "Module unloaded\n");
}

module_init(usb_crypto_drv_init);
module_exit(usb_crypto_drv_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Secure USB crypto mouse driver");
MODULE_AUTHOR("secure_usb_crypto");
MODULE_VERSION("0.1");
MODULE_IMPORT_NS("CRYPTO_INTERNAL");

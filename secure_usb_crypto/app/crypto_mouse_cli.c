#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../include/crypto_mouse_ioctl.h"

#define DEV_PATH "/dev/crypto_mouse"

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s status\n"
            "  %s setkey <hex_key_16_24_or_32_bytes>\n"
            "  %s clearkey\n"
            "  %s write <text_or_hex:0x...>\n"
            "  %s read\n"
            "  %s encrypt\n"
            "  %s decrypt\n"
            "  %s encrypt-file <hex_key> <input_file> <output_file>\n"
            "  %s decrypt-file <hex_key> <input_file> <output_file>\n",
            prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *hex, uint8_t *out, size_t out_max, size_t *out_len)
{
    size_t i;
    size_t len = strlen(hex);

    if (len >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
        len -= 2;
    }

    if (len == 0 || (len % 2) != 0)
        return -1;

    if ((len / 2) > out_max)
        return -1;

    for (i = 0; i < len; i += 2) {
        int hi = hex_val(hex[i]);
        int lo = hex_val(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }

    *out_len = len / 2;
    return 0;
}

static int cmd_status(int fd)
{
    struct crypto_mouse_status st;

    if (ioctl(fd, CRYPTO_MOUSE_IOC_GET_STATUS, &st) < 0) {
        perror("ioctl(GET_STATUS)");
        return 1;
    }

    printf("mouse_present=%u key_ready=%u data_len=%u is_encrypted=%u plain_len=%u\n",
           st.mouse_present, st.key_ready, st.data_len,
           st.is_encrypted, st.plain_len);
    return 0;
}

static int cmd_get_status(int fd, struct crypto_mouse_status *st)
{
    if (ioctl(fd, CRYPTO_MOUSE_IOC_GET_STATUS, st) < 0)
        return -1;
    return 0;
}

static int cmd_setkey(int fd, const char *hex)
{
    struct crypto_mouse_key key_req;
    size_t key_len = 0;

    memset(&key_req, 0, sizeof(key_req));

    if (parse_hex(hex, key_req.key, sizeof(key_req.key), &key_len) != 0) {
        fprintf(stderr, "Invalid key hex format\n");
        return 1;
    }

    key_req.key_len = (uint32_t)key_len;

    if (ioctl(fd, CRYPTO_MOUSE_IOC_SET_KEY, &key_req) < 0) {
        perror("ioctl(SET_KEY)");
        return 1;
    }

    printf("set key ok (len=%zu)\n", key_len);
    return 0;
}

static int cmd_write(int fd, const char *arg)
{
    uint8_t *buf;
    size_t len = 0;
    ssize_t wr;

    buf = malloc(CRYPTO_MOUSE_MAX_DATA);
    if (!buf) {
        perror("malloc");
        return 1;
    }

    if (strncmp(arg, "0x", 2) == 0 || strncmp(arg, "0X", 2) == 0) {
        if (parse_hex(arg, buf, CRYPTO_MOUSE_MAX_DATA, &len) != 0) {
            fprintf(stderr, "Invalid hex payload\n");
            free(buf);
            return 1;
        }
    } else {
        len = strlen(arg);
        if (len > CRYPTO_MOUSE_MAX_DATA) {
            fprintf(stderr, "Payload too large\n");
            free(buf);
            return 1;
        }
        memcpy(buf, arg, len);
    }

    wr = write(fd, buf, len);
    if (wr < 0) {
        perror("write");
        free(buf);
        return 1;
    }

    printf("write ok (%zd bytes)\n", wr);
    free(buf);
    return 0;
}

static int cmd_write_raw(int fd, const uint8_t *data, size_t len)
{
    ssize_t wr;

    wr = write(fd, data, len);
    if (wr < 0)
        return -1;
    if ((size_t)wr != len) {
        errno = EIO;
        return -1;
    }

    return 0;
}

static int cmd_read(int fd)
{
    uint8_t *buf;
    ssize_t rd;
    ssize_t i;

    buf = malloc(CRYPTO_MOUSE_MAX_DATA);
    if (!buf) {
        perror("malloc");
        return 1;
    }

    lseek(fd, 0, SEEK_SET);
    rd = read(fd, buf, CRYPTO_MOUSE_MAX_DATA);
    if (rd < 0) {
        perror("read");
        free(buf);
        return 1;
    }

    printf("read %zd bytes\n", rd);
    printf("hex: ");
    for (i = 0; i < rd; i++)
        printf("%02x", buf[i]);
    printf("\n");

    free(buf);
    return 0;
}

static int cmd_simple_ioctl(int fd, unsigned long req, const char *name)
{
    if (ioctl(fd, req) < 0) {
        perror(name);
        return 1;
    }

    printf("%s ok\n", name);
    return 0;
}

static int read_device_exact(int fd, uint8_t *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t rd = read(fd, buf + off, len - off);
        if (rd < 0)
            return -1;
        if (rd == 0)
            break;
        off += (size_t)rd;
    }

    if (off != len) {
        errno = EIO;
        return -1;
    }

    return 0;
}

static int load_file(const char *path, uint8_t *buf, size_t max_len, size_t *out_len)
{
    struct stat st;
    FILE *fp;
    size_t rd;

    if (stat(path, &st) != 0)
        return -1;

    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }

    if (st.st_size < 0 || (size_t)st.st_size > max_len) {
        errno = EMSGSIZE;
        return -1;
    }

    fp = fopen(path, "rb");
    if (!fp)
        return -1;

    rd = fread(buf, 1, (size_t)st.st_size, fp);
    if (rd != (size_t)st.st_size) {
        fclose(fp);
        errno = EIO;
        return -1;
    }

    fclose(fp);
    *out_len = rd;
    return 0;
}

static int save_file(const char *path, const uint8_t *buf, size_t len)
{
    FILE *fp = fopen(path, "wb");
    size_t wr;

    if (!fp)
        return -1;

    wr = fwrite(buf, 1, len, fp);
    if (wr != len) {
        fclose(fp);
        errno = EIO;
        return -1;
    }

    fclose(fp);
    return 0;
}

static int cmd_file_crypto(int fd, const char *key_hex, const char *in_path,
                           const char *out_path, int do_encrypt)
{
    uint8_t *inbuf;
    uint8_t *outbuf;
    struct crypto_mouse_status st;
    size_t in_len = 0;
    int rd_fd;

    inbuf = malloc(CRYPTO_MOUSE_MAX_DATA);
    outbuf = malloc(CRYPTO_MOUSE_MAX_DATA);
    if (!inbuf || !outbuf) {
        perror("malloc");
        free(inbuf);
        free(outbuf);
        return 1;
    }

    if (load_file(in_path, inbuf, CRYPTO_MOUSE_MAX_DATA, &in_len) != 0) {
        perror("load input file");
        free(inbuf);
        free(outbuf);
        return 1;
    }

    if (cmd_setkey(fd, key_hex) != 0)
    {
        free(inbuf);
        free(outbuf);
        return 1;
    }

    if (cmd_write_raw(fd, inbuf, in_len) != 0) {
        perror("write device");
        free(inbuf);
        free(outbuf);
        return 1;
    }

    if (do_encrypt) {
        if (cmd_simple_ioctl(fd, CRYPTO_MOUSE_IOC_ENCRYPT, "ioctl(ENCRYPT)") != 0)
        {
            free(inbuf);
            free(outbuf);
            return 1;
        }
    } else {
        if (cmd_simple_ioctl(fd, CRYPTO_MOUSE_IOC_DECRYPT, "ioctl(DECRYPT)") != 0)
        {
            free(inbuf);
            free(outbuf);
            return 1;
        }
    }

    if (cmd_get_status(fd, &st) != 0) {
        perror("ioctl(GET_STATUS)");
        free(inbuf);
        free(outbuf);
        return 1;
    }

    if (st.data_len > CRYPTO_MOUSE_MAX_DATA) {
        fprintf(stderr, "driver data length too large: %u\n", st.data_len);
        free(inbuf);
        free(outbuf);
        return 1;
    }

    rd_fd = open(DEV_PATH, O_RDONLY);
    if (rd_fd < 0) {
        perror("open for read");
        free(inbuf);
        free(outbuf);
        return 1;
    }

    if (read_device_exact(rd_fd, outbuf, st.data_len) != 0) {
        close(rd_fd);
        perror("read device");
        free(inbuf);
        free(outbuf);
        return 1;
    }
    close(rd_fd);

    if (save_file(out_path, outbuf, st.data_len) != 0) {
        perror("save output file");
        free(inbuf);
        free(outbuf);
        return 1;
    }

    printf("file %s ok: %s -> %s (%u bytes)\n",
           do_encrypt ? "encrypt" : "decrypt",
           in_path, out_path, st.data_len);
    free(inbuf);
    free(outbuf);
    return 0;
}

int main(int argc, char **argv)
{
    int fd;
    int rc;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    fd = open(DEV_PATH, O_RDWR);
    if (fd < 0) {
        perror("open /dev/crypto_mouse");
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        rc = cmd_status(fd);
        close(fd);
        return rc;
    }

    if (strcmp(argv[1], "setkey") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            close(fd);
            return 1;
        }
        rc = cmd_setkey(fd, argv[2]);
        close(fd);
        return rc;
    }

    if (strcmp(argv[1], "clearkey") == 0) {
        rc = cmd_simple_ioctl(fd, CRYPTO_MOUSE_IOC_CLEAR_KEY, "ioctl(CLEAR_KEY)");
        close(fd);
        return rc;
    }

    if (strcmp(argv[1], "write") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            close(fd);
            return 1;
        }
        rc = cmd_write(fd, argv[2]);
        close(fd);
        return rc;
    }

    if (strcmp(argv[1], "read") == 0) {
        rc = cmd_read(fd);
        close(fd);
        return rc;
    }

    if (strcmp(argv[1], "encrypt") == 0) {
        rc = cmd_simple_ioctl(fd, CRYPTO_MOUSE_IOC_ENCRYPT, "ioctl(ENCRYPT)");
        close(fd);
        return rc;
    }

    if (strcmp(argv[1], "decrypt") == 0) {
        rc = cmd_simple_ioctl(fd, CRYPTO_MOUSE_IOC_DECRYPT, "ioctl(DECRYPT)");
        close(fd);
        return rc;
    }

    if (strcmp(argv[1], "encrypt-file") == 0) {
        if (argc != 5) {
            usage(argv[0]);
            close(fd);
            return 1;
        }
        rc = cmd_file_crypto(fd, argv[2], argv[3], argv[4], 1);
        close(fd);
        return rc;
    }

    if (strcmp(argv[1], "decrypt-file") == 0) {
        if (argc != 5) {
            usage(argv[0]);
            close(fd);
            return 1;
        }
        rc = cmd_file_crypto(fd, argv[2], argv[3], argv[4], 0);
        close(fd);
        return rc;
    }

    usage(argv[0]);
    close(fd);
    return 1;
}

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../include/crypto_mouse_ioctl.h"

#define DEV_PATH "/dev/crypto_mouse"
/* Dau hieu de nhan biet file .enc theo dinh dang chunk moi. */
#define CHUNK_STREAM_MAGIC "CMCHUNK1"
#define CHUNK_STREAM_MAGIC_LEN 8
/* Moi chunk plain de du choi cho padding PKCS#7 trong driver. */
#define ENC_CHUNK_PLAIN_MAX (CRYPTO_MOUSE_MAX_DATA - 16)

/*
 * Note nhanh ve luong du lieu (de de hinh dung luc debug):
 * Mouse -> driver kernel -> buffer trong /dev/crypto_mouse -> user-space CLI ->
 * nap key Camellia -> ioctl encrypt/decrypt -> doc lai ket qua.
 *
 * CLI la lop cau noi user-space <-> driver:
 * - Lenh don: status/setkey/write/read/encrypt/decrypt.
 * - Lenh file: cat file lon thanh chunk, moi chunk gui qua /dev/crypto_mouse.
 * - Decrypt tuong thich 2 dinh dang: chunk moi + legacy 1 blob cu.
 */

/*
 * Muc dich: in huong dan su dung CLI.
 * Khi goi: luc thieu tham so hoac command khong hop le.
 */
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

/*
 * Muc dich: doi 1 ky tu hex thanh gia tri so.
 * Khi goi: ben trong parse_hex, khong dung truc tiep tu main.
 */
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

/*
 * Muc dich: parse chuoi hex vao byte buffer.
 * Khi goi: luc set key hoac write payload dang 0x... .
 */
static int parse_hex(const char *hex, uint8_t *out, size_t out_max, size_t *out_len)
{
    size_t i;
    size_t len = strlen(hex);

    /* Co the nhan key/payload dang 0x... hoac chuoi hex thuong. */
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

/*
 * Muc dich: lay status tu driver va in theo key=value.
 * Khi goi: command "status".
 */
static int cmd_status(int fd)
{
    struct crypto_mouse_status st;

    if (ioctl(fd, CRYPTO_MOUSE_IOC_GET_STATUS, &st) < 0) {
        perror("ioctl(GET_STATUS)");
        return 1;
    }

    /* In ra state de GUI/script co the parse theo key=value. */
    printf("mouse_present=%u key_ready=%u data_len=%u is_encrypted=%u plain_len=%u\n",
           st.mouse_present, st.key_ready, st.data_len,
           st.is_encrypted, st.plain_len);
    return 0;
}

/*
 * Muc dich: helper nho de lay status ma khong in ra man hinh.
 * Khi goi: trong cac command file encrypt/decrypt can data_len.
 */
static int cmd_get_status(int fd, struct crypto_mouse_status *st)
{
    if (ioctl(fd, CRYPTO_MOUSE_IOC_GET_STATUS, st) < 0)
        return -1;
    return 0;
}

/*
 * Muc dich: nap key Camellia vao driver.
 * Khi goi: command "setkey" va truoc cac command file crypto.
 */
static int cmd_setkey(int fd, const char *hex)
{
    struct crypto_mouse_key key_req;
    size_t key_len = 0;

    /* key_req la packet gui qua ioctl(SET_KEY), gom key bytes + key_len. */
    memset(&key_req, 0, sizeof(key_req));

    /* Key hop le: 16/24/32 bytes (Camellia-128/192/256). */
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

/*
 * Muc dich: ghi plain payload vao buffer trong driver.
 * Khi goi: command "write" (text thuong hoac hex).
 */
static int cmd_write(int fd, const char *arg)
{
    uint8_t *buf;
    size_t len = 0;
    ssize_t wr;

    /* buf la vung dem tam phia user-space truoc khi write vao device. */
    buf = malloc(CRYPTO_MOUSE_MAX_DATA);
    if (!buf) {
        perror("malloc");
        return 1;
    }

    /* Ho tro 2 dang input: text thuong hoac byte hex (0x...). */
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

/*
 * Muc dich: ghi du chinh xac "len" byte vao device.
 * Khi goi: duong xu ly file chunk, can strict hon cmd_write.
 */
static int cmd_write_raw(int fd, const uint8_t *data, size_t len)
{
    ssize_t wr;

    wr = write(fd, data, len);
    if (wr < 0)
        return -1;
    /* Lenh file can ghi du so byte, thieu 1 byte cung xem la loi I/O. */
    if ((size_t)wr != len) {
        errno = EIO;
        return -1;
    }

    return 0;
}

/*
 * Muc dich: doc noi dung hien co trong buffer driver va in hex.
 * Khi goi: command "read".
 */
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

    /* Dua offset ve 0 de doc lai tu dau buffer driver. */
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

/*
 * Muc dich: wrapper cho cac ioctl don (co in thong bao ok).
 * Khi goi: command encrypt/decrypt/clearkey.
 */
static int cmd_simple_ioctl(int fd, unsigned long req, const char *name)
{
    if (ioctl(fd, req) < 0) {
        perror(name);
        return 1;
    }

    printf("%s ok\n", name);
    return 0;
}

/*
 * Muc dich: wrapper ioctl im lang (chi bao loi), dung cho pipeline file.
 * Khi goi: encrypt/decrypt theo chunk de tranh spam output.
 */
static int cmd_simple_ioctl_quiet(int fd, unsigned long req, const char *name)
{
    if (ioctl(fd, req) < 0) {
        perror(name);
        return 1;
    }

    return 0;
}

/*
 * Muc dich: doc du so byte mong muon tu /dev/crypto_mouse.
 * Khi goi: sau moi lan ioctl encrypt/decrypt trong file mode.
 */
static int read_device_exact(int fd, uint8_t *buf, size_t len)
{
    size_t off = 0;

    /* Read vong lap de gom du len byte can lay tu /dev/crypto_mouse. */
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

/*
 * Muc dich: reset offset fd doc ve 0 roi doc 1 blob dung do dai.
 * Khi goi: moi lan can lay blob moi tu driver.
 */
static int read_device_blob_fd(int rd_fd, uint8_t *buf, size_t len)
{
    if (lseek(rd_fd, 0, SEEK_SET) < 0)
        return -1;

    return read_device_exact(rd_fd, buf, len);
}

/*
 * Muc dich: fread cho den khi du byte, tranh doc nua chung.
 * Khi goi: doc chunk header/data va legacy blob tu file.
 */
static int read_exact_file(FILE *fp, uint8_t *buf, size_t len)
{
    size_t off = 0;

    /* fread co the tra ve it hon len, nen can loop den khi du byte. */
    while (off < len) {
        size_t rd = fread(buf + off, 1, len - off, fp);
        if (rd == 0) {
            if (ferror(fp))
                return -1;
            break;
        }
        off += rd;
    }

    if (off != len) {
        errno = EIO;
        return -1;
    }

    return 0;
}

/*
 * Muc dich: fwrite du so byte, fail som neu ghi thieu.
 * Khi goi: ghi header/chunk/output file.
 */
static int write_exact_file(FILE *fp, const uint8_t *buf, size_t len)
{
    if (fwrite(buf, 1, len, fp) != len) {
        errno = EIO;
        return -1;
    }

    return 0;
}

/*
 * Muc dich: ghi metadata cua 1 chunk (plain_len + cipher_len).
 * Khi goi: moi chunk sau khi encrypt xong.
 */
static int write_chunk_header(FILE *fp, uint32_t plain_len, uint32_t cipher_len)
{
    uint32_t hdr[2];

    /* Header chunk luu little-endian de portable giua he kien truc. */
    hdr[0] = htole32(plain_len);
    hdr[1] = htole32(cipher_len);

    return write_exact_file(fp, (const uint8_t *)hdr, sizeof(hdr));
}

/*
 * Muc dich: doc header chunk va parse little-endian.
 * Khi goi: vong lap decrypt chunked.
 */
static int read_chunk_header(FILE *fp, uint32_t *plain_len, uint32_t *cipher_len,
                             int *is_eof)
{
    uint8_t raw[8];
    uint32_t plain_le;
    uint32_t cipher_le;
    size_t rd;

    /* EOF dung quy cach: fread=0 va khong co ferror. */
    *is_eof = 0;
    rd = fread(raw, 1, sizeof(raw), fp);
    if (rd == 0) {
        if (ferror(fp))
            return -1;
        *is_eof = 1;
        return 0;
    }

    if (rd != sizeof(raw)) {
        errno = EINVAL;
        return -1;
    }

    /* Dung memcpy de tranh loi can le bo nho/strict-aliasing tren mot so KTS. */
    memcpy(&plain_le, &raw[0], sizeof(plain_le));
    memcpy(&cipher_le, &raw[4], sizeof(cipher_le));
    *plain_len = le32toh(plain_le);
    *cipher_len = le32toh(cipher_le);
    return 0;
}

/*
 * Muc dich: ghi 1 blob du lieu ra file output.
 * Khi goi: decrypt legacy xong can save plain.
 */
static int save_file(const char *path, const uint8_t *buf, size_t len)
{
    FILE *fp = fopen(path, "wb");

    if (!fp)
        return -1;

    if (write_exact_file(fp, buf, len) != 0) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

/*
 * Muc dich: encrypt file lon theo stream chunk de khong bi limit MAX_DATA.
 * Khi goi: command "encrypt-file".
 */
static int cmd_file_encrypt_chunked(int fd, const char *key_hex,
                                    const char *in_path, const char *out_path)
{
    uint8_t *inbuf;
    uint8_t *outbuf;
    struct crypto_mouse_status st;
    FILE *fin;
    FILE *fout;
    size_t in_len;
    int rd_fd = -1;
    int rc = 1;

    /*
     * Dinh dang output chunked:
     * [magic 8B][plain_len 4B][cipher_len 4B][cipher bytes]...
     * Moi chunk plain toi da MAX_DATA-16 de sau padding van <= MAX_DATA.
     */
    /* inbuf la plain chunk doc tu file; outbuf la cipher chunk doc nguoc tu driver. */
    inbuf = malloc(ENC_CHUNK_PLAIN_MAX);
    outbuf = malloc(CRYPTO_MOUSE_MAX_DATA);
    if (!inbuf || !outbuf) {
        perror("malloc");
        free(inbuf);
        free(outbuf);
        return 1;
    }

    fin = fopen(in_path, "rb");
    if (!fin) {
        perror("open input file");
        free(inbuf);
        free(outbuf);
        return 1;
    }

    fout = fopen(out_path, "wb");
    if (!fout) {
        perror("open output file");
        fclose(fin);
        free(inbuf);
        free(outbuf);
        return 1;
    }

    if (write_exact_file(fout, (const uint8_t *)CHUNK_STREAM_MAGIC,
                         CHUNK_STREAM_MAGIC_LEN) != 0) {
        perror("write output header");
        goto out;
    }

    if (cmd_setkey(fd, key_hex) != 0) {
        goto out;
    }

    /* Tai su dung 1 read-fd cho toan bo vong lap chunk de giam syscall. */
    rd_fd = open(DEV_PATH, O_RDONLY);
    if (rd_fd < 0) {
        perror("open read device");
        goto out;
    }

    /*
     * Pipeline moi chunk:
     * file -> write(/dev/crypto_mouse) -> ioctl(ENCRYPT) -> read(/dev/crypto_mouse) -> file .enc
     */
    while ((in_len = fread(inbuf, 1, ENC_CHUNK_PLAIN_MAX, fin)) > 0) {
        if (cmd_write_raw(fd, inbuf, in_len) != 0) {
            perror("write device");
            goto out;
        }

        if (cmd_simple_ioctl_quiet(fd, CRYPTO_MOUSE_IOC_ENCRYPT, "ioctl(ENCRYPT)") != 0)
            goto out;

        if (cmd_get_status(fd, &st) != 0) {
            perror("ioctl(GET_STATUS)");
            goto out;
        }

        /* data_len luc nay la kich thuoc cipher sau PKCS#7 va phai > 0. */
        if (st.data_len > CRYPTO_MOUSE_MAX_DATA || st.data_len == 0) {
            fprintf(stderr, "driver data length invalid: %u\n", st.data_len);
            goto out;
        }

        if (read_device_blob_fd(rd_fd, outbuf, st.data_len) != 0) {
            perror("read device");
            goto out;
        }

        if (write_chunk_header(fout, (uint32_t)in_len, st.data_len) != 0) {
            perror("write chunk header");
            goto out;
        }

        if (write_exact_file(fout, outbuf, st.data_len) != 0) {
            perror("write chunk data");
            goto out;
        }
    }

    if (ferror(fin)) {
        perror("read input file");
        goto out;
    }

    rc = 0;

out:
    if (rd_fd >= 0)
        close(rd_fd);
    fclose(fin);
    fclose(fout);
    free(inbuf);
    free(outbuf);

    /* Loi bat ky buoc nao thi xoa file output de tranh de lai file dang do. */
    if (rc != 0)
        unlink(out_path);

    if (rc == 0)
        printf("file encrypt ok (chunked): %s -> %s\n", in_path, out_path);

    return rc;
}

/*
 * Muc dich: decrypt dinh dang cu (ca file la 1 blob cipher <= MAX_DATA).
 * Khi goi: fallback khi file khong dung chunk magic.
 */
static int cmd_file_decrypt_legacy(int fd, const char *key_hex, const char *in_path,
                                   const char *out_path)
{
    uint8_t *inbuf;
    uint8_t *outbuf;
    struct stat st_file;
    struct crypto_mouse_status st;
    size_t in_len;
    int rd_fd = -1;

    /* Legacy format: ca file cipher nam tron trong 1 blob <= MAX_DATA. */
    if (stat(in_path, &st_file) != 0) {
        perror("stat input file");
        return 1;
    }

    if (!S_ISREG(st_file.st_mode) || st_file.st_size < 0 ||
        (size_t)st_file.st_size > CRYPTO_MOUSE_MAX_DATA) {
        fprintf(stderr, "legacy input size unsupported\n");
        return 1;
    }

    in_len = (size_t)st_file.st_size;
    /* inbuf giu blob cipher; outbuf giu ket qua plain tra ve tu driver. */
    inbuf = malloc(CRYPTO_MOUSE_MAX_DATA);
    outbuf = malloc(CRYPTO_MOUSE_MAX_DATA);
    if (!inbuf || !outbuf) {
        perror("malloc");
        free(inbuf);
        free(outbuf);
        return 1;
    }

    {
        FILE *fp = fopen(in_path, "rb");
        if (!fp) {
            perror("open input file");
            free(inbuf);
            free(outbuf);
            return 1;
        }
        if (read_exact_file(fp, inbuf, in_len) != 0) {
            perror("read input file");
            fclose(fp);
            free(inbuf);
            free(outbuf);
            return 1;
        }
        fclose(fp);
    }

    if (cmd_setkey(fd, key_hex) != 0) {
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

    if (cmd_simple_ioctl(fd, CRYPTO_MOUSE_IOC_DECRYPT, "ioctl(DECRYPT)") != 0) {
        free(inbuf);
        free(outbuf);
        return 1;
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
        perror("open read device");
        free(inbuf);
        free(outbuf);
        return 1;
    }

    if (read_device_blob_fd(rd_fd, outbuf, st.data_len) != 0) {
        perror("read device");
        close(rd_fd);
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

    printf("file decrypt ok (legacy): %s -> %s (%u bytes)\n",
           in_path, out_path, st.data_len);
    free(inbuf);
    free(outbuf);
    return 0;
}

/*
 * Muc dich: decrypt dinh dang chunk moi (co magic + header moi chunk).
 * Khi goi: command "decrypt-file", uu tien thu chunked truoc.
 */
static int cmd_file_decrypt_chunked(int fd, const char *key_hex,
                                    const char *in_path, const char *out_path)
{
    uint8_t *inbuf;
    uint8_t *outbuf;
    uint8_t magic[CHUNK_STREAM_MAGIC_LEN];
    struct crypto_mouse_status st;
    FILE *fin;
    FILE *fout;
    int rd_fd = -1;
    int rc = 1;

    /* inbuf = chunk cipher doc tu file, outbuf = chunk plain lay tu driver. */
    inbuf = malloc(CRYPTO_MOUSE_MAX_DATA);
    outbuf = malloc(CRYPTO_MOUSE_MAX_DATA);
    if (!inbuf || !outbuf) {
        perror("malloc");
        free(inbuf);
        free(outbuf);
        return 1;
    }

    fin = fopen(in_path, "rb");
    if (!fin) {
        perror("open input file");
        free(inbuf);
        free(outbuf);
        return 1;
    }

    /* Thu doc magic de nhan dien format chunk moi. */
    if (read_exact_file(fin, magic, sizeof(magic)) != 0) {
        fclose(fin);
        free(inbuf);
        free(outbuf);
        /* Khong co magic -> fallback giai ma theo legacy. */
        return cmd_file_decrypt_legacy(fd, key_hex, in_path, out_path);
    }

    if (memcmp(magic, CHUNK_STREAM_MAGIC, CHUNK_STREAM_MAGIC_LEN) != 0) {
        fclose(fin);
        free(inbuf);
        free(outbuf);
        /* Magic khong khop -> cung fallback legacy de tuong thich nguoc. */
        return cmd_file_decrypt_legacy(fd, key_hex, in_path, out_path);
    }

    fout = fopen(out_path, "wb");
    if (!fout) {
        perror("open output file");
        fclose(fin);
        free(inbuf);
        free(outbuf);
        return 1;
    }

    if (cmd_setkey(fd, key_hex) != 0)
        goto out;

    /* Tai su dung 1 read-fd cho toan bo vong lap chunk de giam syscall. */
    rd_fd = open(DEV_PATH, O_RDONLY);
    if (rd_fd < 0) {
        perror("open read device");
        goto out;
    }

    for (;;) {
        uint32_t plain_len = 0;
        uint32_t cipher_len = 0;
        int is_eof = 0;

        if (read_chunk_header(fin, &plain_len, &cipher_len, &is_eof) != 0) {
            perror("read chunk header");
            goto out;
        }

        /* EOF la ket thuc stream hop le. */
        if (is_eof)
            break;

        /* Validate header truoc khi cap phat/doc chunk de tranh file loi. */
        if (cipher_len == 0 || cipher_len > CRYPTO_MOUSE_MAX_DATA ||
            (cipher_len % 16) != 0 || plain_len > cipher_len) {
            fprintf(stderr, "invalid chunk header: plain=%u cipher=%u\n",
                    plain_len, cipher_len);
            goto out;
        }

        if (read_exact_file(fin, inbuf, cipher_len) != 0) {
            perror("read chunk data");
            goto out;
        }

        if (cmd_write_raw(fd, inbuf, cipher_len) != 0) {
            perror("write device");
            goto out;
        }

        if (cmd_simple_ioctl_quiet(fd, CRYPTO_MOUSE_IOC_DECRYPT, "ioctl(DECRYPT)") != 0)
            goto out;

        if (cmd_get_status(fd, &st) != 0) {
            perror("ioctl(GET_STATUS)");
            goto out;
        }

        /* Driver phai tra dung plain_len, sai la co van de key/data. */
        if (st.data_len > CRYPTO_MOUSE_MAX_DATA || st.data_len != plain_len) {
            fprintf(stderr,
                    "driver/plain mismatch: plain=%u driver=%u\n",
                    plain_len, st.data_len);
            goto out;
        }

        if (read_device_blob_fd(rd_fd, outbuf, st.data_len) != 0) {
            perror("read device");
            goto out;
        }

        if (write_exact_file(fout, outbuf, st.data_len) != 0) {
            perror("write output file");
            goto out;
        }
    }

    rc = 0;

out:
    if (rd_fd >= 0)
        close(rd_fd);
    fclose(fin);
    fclose(fout);
    free(inbuf);
    free(outbuf);

    if (rc != 0)
        unlink(out_path);

    if (rc == 0)
        printf("file decrypt ok (chunked): %s -> %s\n", in_path, out_path);

    return rc;
}

/*
 * Muc dich: diem dispatch chung cho file encrypt/decrypt.
 * Khi goi: tu main sau khi parse command encrypt-file/decrypt-file.
 */
static int cmd_file_crypto(int fd, const char *key_hex, const char *in_path,
                           const char *out_path, int do_encrypt)
{
    /* Ham wrapper de gom encrypt/decrypt file ve 1 diem dispatch. */
    if (do_encrypt)
        return cmd_file_encrypt_chunked(fd, key_hex, in_path, out_path);

    return cmd_file_decrypt_chunked(fd, key_hex, in_path, out_path);
}

/*
 * Muc dich: entrypoint CLI, parse argv va goi command phu hop.
 * Khi goi: moi lan chay binary crypto_mouse_cli.
 */
int main(int argc, char **argv)
{
    int fd;
    int rc;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    /* Mo /dev/crypto_mouse 1 lan, roi dispatch theo command argv[1]. */
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

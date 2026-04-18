#include "pes.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/sha.h>

static const char *object_type_name(ObjectType type) {
    switch (type) {
        case OBJ_BLOB: return "blob";
        case OBJ_TREE: return "tree";
        case OBJ_COMMIT: return "commit";
        default: return NULL;
    }
}

static int object_type_from_name(const char *name, ObjectType *type_out) {
    if (strcmp(name, "blob") == 0) {
        *type_out = OBJ_BLOB;
        return 0;
    }
    if (strcmp(name, "tree") == 0) {
        *type_out = OBJ_TREE;
        return 0;
    }
    if (strcmp(name, "commit") == 0) {
        *type_out = OBJ_COMMIT;
        return 0;
    }
    return -1;
}

void hash_to_hex(const ObjectID *id, char *hex_out) {
    static const char digits[] = "0123456789abcdef";

    for (int i = 0; i < HASH_SIZE; i++) {
        hex_out[i * 2] = digits[id->hash[i] >> 4];
        hex_out[i * 2 + 1] = digits[id->hash[i] & 0x0f];
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        char hi = (char)tolower((unsigned char)hex[i * 2]);
        char lo = (char)tolower((unsigned char)hex[i * 2 + 1]);
        int hi_val;
        int lo_val;

        if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo)) {
            return -1;
        }

        hi_val = (hi <= '9') ? (hi - '0') : (hi - 'a' + 10);
        lo_val = (lo <= '9') ? (lo - '0') : (lo - 'a' + 10);
        id_out->hash[i] = (uint8_t)((hi_val << 4) | lo_val);
    }
    return 0;
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_name = object_type_name(type);
    char header[64];
    int header_len;
    size_t total_len;
    unsigned char *buffer;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    char hex[HASH_HEX_SIZE + 1];
    char dir[512];
    char path[512];
    char tmp_path[520];
    int fd;
    ssize_t written;

    if (!type_name) return -1;

    header_len = snprintf(header, sizeof(header), "%s %zu", type_name, len) + 1;
    if (header_len <= 0 || (size_t)header_len > sizeof(header)) return -1;

    total_len = (size_t)header_len + len;
    buffer = malloc(total_len);
    if (!buffer) return -1;

    memcpy(buffer, header, (size_t)header_len);
    if (len > 0 && data != NULL) {
        memcpy(buffer + header_len, data, len);
    }

    SHA256(buffer, total_len, digest);
    memcpy(id_out->hash, digest, HASH_SIZE);
    hash_to_hex(id_out, hex);

    mkdir(PES_DIR, 0755);
    mkdir(OBJECTS_DIR, 0755);
    snprintf(dir, sizeof(dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(dir, 0755);

    object_path(id_out, path, sizeof(path));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(buffer);
        return -1;
    }

    written = write(fd, buffer, total_len);
    if (written != (ssize_t)total_len) {
        int saved_errno = errno;
        close(fd);
        unlink(tmp_path);
        free(buffer);
        errno = saved_errno;
        return -1;
    }

    if (fsync(fd) != 0) {
        int saved_errno = errno;
        close(fd);
        unlink(tmp_path);
        free(buffer);
        errno = saved_errno;
        return -1;
    }

    if (close(fd) != 0) {
        int saved_errno = errno;
        unlink(tmp_path);
        free(buffer);
        errno = saved_errno;
        return -1;
    }

    if (rename(tmp_path, path) != 0) {
        int saved_errno = errno;
        unlink(tmp_path);
        free(buffer);
        errno = saved_errno;
        return -1;
    }

    free(buffer);
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    struct stat st;
    int fd;
    unsigned char *buffer;
    ssize_t read_bytes;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    char computed_hex[HASH_HEX_SIZE + 1];
    char expected_hex[HASH_HEX_SIZE + 1];
    char *nul;
    char type_name[16];
    size_t payload_len;

    object_path(id, path, sizeof(path));
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }

    buffer = malloc((size_t)st.st_size);
    if (!buffer) {
        close(fd);
        return -1;
    }

    read_bytes = read(fd, buffer, (size_t)st.st_size);
    close(fd);
    if (read_bytes != st.st_size) {
        free(buffer);
        return -1;
    }

    SHA256(buffer, (size_t)st.st_size, digest);
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(computed_hex + i * 2, "%02x", digest[i]);
    }
    computed_hex[HASH_HEX_SIZE] = '\0';

    hash_to_hex(id, expected_hex);
    if (strcmp(computed_hex, expected_hex) != 0) {
        free(buffer);
        return -1;
    }

    nul = memchr(buffer, '\0', (size_t)st.st_size);
    if (!nul) {
        free(buffer);
        return -1;
    }

    if (sscanf((char *)buffer, "%15s %zu", type_name, len_out) != 2) {
        free(buffer);
        return -1;
    }

    if (object_type_from_name(type_name, type_out) != 0) {
        free(buffer);
        return -1;
    }

    payload_len = (size_t)st.st_size - (size_t)((nul + 1) - (char *)buffer);
    if (payload_len != *len_out) {
        free(buffer);
        return -1;
    }

    *data_out = malloc(*len_out);
    if (!*data_out && *len_out != 0) {
        free(buffer);
        return -1;
    }

    if (*len_out > 0) {
        memcpy(*data_out, nul + 1, *len_out);
    }

    free(buffer);
    return 0;
}

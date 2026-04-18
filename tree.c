// tree.c — Tree object serialization and construction

#include "tree.h"
#include "index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

static uint32_t normalize_mode(uint32_t mode) {
    if ((mode & S_IFMT) == S_IFDIR) return MODE_DIR;
    if (mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// PROVIDED (unchanged)
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', (size_t)(end - ptr));
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = (size_t)(space - ptr);
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = (uint32_t)strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', (size_t)(end - ptr));
        if (!null_byte) return -1;

        size_t name_len = (size_t)(null_byte - ptr);
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }

    return ptr == end ? 0 : -1;
}

static int compare_tree_entries(const void *a, const void *b) {
    const TreeEntry *ea = (const TreeEntry *)a;
    const TreeEntry *eb = (const TreeEntry *)b;
    return strcmp(ea->name, eb->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    Tree sorted = *tree;
    size_t capacity = 0;
    uint8_t *buffer;
    size_t offset = 0;

    for (int i = 0; i < tree->count; i++) {
        capacity += strlen(tree->entries[i].name) + HASH_SIZE + 16;
    }

    buffer = malloc(capacity > 0 ? capacity : 1);
    if (!buffer) return -1;

    qsort(sorted.entries, (size_t)sorted.count, sizeof(TreeEntry), compare_tree_entries);

    for (int i = 0; i < sorted.count; i++) {
        const TreeEntry *entry = &sorted.entries[i];
        int n = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        if (n < 0) {
            free(buffer);
            return -1;
        }
        offset += (size_t)n + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

static int tree_has_entry(const Tree *tree, const char *name) {
    for (int i = 0; i < tree->count; i++) {
        if (strcmp(tree->entries[i].name, name) == 0) return 1;
    }
    return 0;
}

static int build_tree_level(const Index *index, const char *base_path, ObjectID *tree_id_out) {
    Tree tree = {0};
    size_t base_len = base_path ? strlen(base_path) : 0;

    for (int i = 0; i < index->count; i++) {
        const IndexEntry *entry = &index->entries[i];
        const char *relative;
        const char *slash;

        if (base_path) {
            if (strncmp(entry->path, base_path, base_len) != 0 || entry->path[base_len] != '/') {
                continue;
            }
            relative = entry->path + base_len + 1;
        } else {
            relative = entry->path;
        }

        slash = strchr(relative, '/');
        if (!slash) {
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            tree.entries[tree.count].mode = normalize_mode(entry->mode);
            tree.entries[tree.count].hash = entry->hash;
            snprintf(tree.entries[tree.count].name, sizeof(tree.entries[tree.count].name), "%s", relative);
            tree.count++;
            continue;
        }

        size_t dir_len = (size_t)(slash - relative);
        char dirname[256];
        char child_base[512];
        ObjectID child_id;

        if (dir_len >= sizeof(dirname)) return -1;
        memcpy(dirname, relative, dir_len);
        dirname[dir_len] = '\0';

        if (tree_has_entry(&tree, dirname)) continue;
        if (tree.count >= MAX_TREE_ENTRIES) return -1;

        if (base_path) {
            snprintf(child_base, sizeof(child_base), "%s/%s", base_path, dirname);
        } else {
            snprintf(child_base, sizeof(child_base), "%s", dirname);
        }

        if (build_tree_level(index, child_base, &child_id) != 0) return -1;

        tree.entries[tree.count].mode = MODE_DIR;
        tree.entries[tree.count].hash = child_id;
        snprintf(tree.entries[tree.count].name, sizeof(tree.entries[tree.count].name), "%s", dirname);
        tree.count++;
    }

    {
        void *data = NULL;
        size_t len = 0;

        if (tree_serialize(&tree, &data, &len) != 0) return -1;
        if (object_write(OBJ_TREE, data, len, tree_id_out) != 0) {
            free(data);
            return -1;
        }
        free(data);
    }

    return 0;
}

int tree_from_index(ObjectID *id_out) {
    Index *index = malloc(sizeof(Index));
    int rc;

    if (!index) return -1;
    if (index_load(index) != 0) {
        free(index);
        return -1;
    }

    rc = build_tree_level(index, NULL, id_out);
    free(index);
    return rc;
}

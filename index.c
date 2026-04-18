#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MODE_FILE 0100644
#define MODE_EXEC 0100755

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

static int cmp_index_entries(const void *a, const void *b) {
    const IndexEntry *ia = (const IndexEntry *)a;
    const IndexEntry *ib = (const IndexEntry *)b;
    return strcmp(ia->path, ib->path);
}

static uint32_t normalized_file_mode(const struct stat *st) {
    return (st->st_mode & S_IXUSR) ? MODE_EXEC : MODE_FILE;
}

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            return &index->entries[i];
        }
    }
    return NULL;
}

int index_load(Index *index) {
    FILE *f = fopen(INDEX_FILE, "r");

    index->count = 0;

    if (!f) {
        return 0;
    }

    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry entry;
        char hex[HASH_HEX_SIZE + 1];
        unsigned long long mtime = 0;
        unsigned int size = 0;

        int fields = fscanf(f, "%o %64s %llu %u %511[^\n]\n",
                            &entry.mode, hex, &mtime, &size, entry.path);

        if (fields == EOF) {
            break;
        }

        if (fields != 5) {
            fclose(f);
            return -1;
        }

        if (hex_to_hash(hex, &entry.hash) != 0) {
            fclose(f);
            return -1;
        }

        entry.mtime_sec = (uint64_t)mtime;
        entry.size = size;
        index->entries[index->count++] = entry;
    }

    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    FILE *f;
    Index sorted = *index;

    mkdir(PES_DIR, 0755);

    qsort(sorted.entries, (size_t)sorted.count, sizeof(IndexEntry), cmp_index_entries);

    f = fopen(".pes/index.tmp", "w");
    if (!f) return -1;

    for (int i = 0; i < sorted.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted.entries[i].hash, hex);

        fprintf(f, "%o %s %llu %u %s\n",
                sorted.entries[i].mode,
                hex,
                (unsigned long long)sorted.entries[i].mtime_sec,
                sorted.entries[i].size,
                sorted.entries[i].path);
    }

    if (fclose(f) != 0) return -1;

    return rename(".pes/index.tmp", INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    struct stat st;
    FILE *f;
    void *data;
    size_t read_len;
    ObjectID id;
    IndexEntry *entry;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }

    f = fopen(path, "rb");
    if (!f) return -1;

    data = malloc((size_t)st.st_size > 0 ? (size_t)st.st_size : 1);
    if (!data) {
        fclose(f);
        return -1;
    }

    read_len = fread(data, 1, (size_t)st.st_size, f);
    fclose(f);

    if (read_len != (size_t)st.st_size) {
        free(data);
        return -1;
    }

    if (object_write(OBJ_BLOB, data, (size_t)st.st_size, &id) != 0) {
        free(data);
        return -1;
    }

    free(data);

    entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
    }

    entry->mode = normalized_file_mode(&st);
    entry->hash = id;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size = (uint32_t)st.st_size;
    snprintf(entry->path, sizeof(entry->path), "%s", path);

    return index_save(index);
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            memmove(&index->entries[i],
                    &index->entries[i + 1],
                    (size_t)(index->count - i - 1) * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
    }
    printf("\nUnstaged changes:\n");
    printf("\nUntracked files:\n");
    return 0;
}

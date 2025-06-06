// SPDX-License-Identifier: MIT
// List of images.
// Copyright (C) 2022 Artem Senichev <artemsen@gmail.com>

#include "imglist.h"

#include "application.h"
#include "array.h"
#include "buildcfg.h"
#include "fs.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/** Order of file list. */
enum list_order {
    order_none,    ///< Unsorted (system depended)
    order_alpha,   ///< Lexicographic sort
    order_numeric, ///< Numeric sort
    order_mtime,   ///< Modification time sort
    order_size,    ///< Size sort
    order_random   ///< Random order
};

// clang-format off
/** Order names. */
static const char* order_names[] = {
    [order_none] = "none",
    [order_alpha] = "alpha",
    [order_numeric] = "numeric",
    [order_mtime] = "mtime",
    [order_size] = "size",
    [order_random] = "random",
};
// clang-format on

/** Context of the image list. */
struct image_list {
    struct image* images; ///< Image list
    size_t size;          ///< Size of image list
    pthread_mutex_t lock; ///< List lock

    enum list_order order; ///< File list order
    bool reverse;          ///< Reverse order flag
    bool loop;             ///< File list loop mode
    bool recursive;        ///< Read directories recursively
    bool all_files;        ///< Open all files from the same directory
    bool from_file;        ///< Interpret input files as lists
};

/** Global image list instance. */
static struct image_list ctx;

/**
 * Search the right place to insert new entry according to sort order.
 * @param img new image entry to insert
 * @return image entry in the list that should be used as "before" position
 */
static struct image* ordered_position(const struct image* img)
{
    struct image* pos = NULL;

    if (ctx.order == order_none) {
        // unsorted
    } else if (ctx.order == order_random) {
        size_t index = rand() % ctx.size;
        list_for_each(ctx.images, struct image, it) {
            if (!index--) {
                pos = it;
                break;
            }
        }
    } else {
        list_for_each(ctx.images, struct image, it) {
            ssize_t cmp = 0;
            switch (ctx.order) {
                case order_alpha:
                    cmp = strcoll(img->source, it->source);
                    break;
                case order_numeric: {
                    const char* a = img->source;
                    const char* b = it->source;
                    while (cmp == 0 && *a && *b) {
                        if (isdigit(*a) && isdigit(*b)) {
                            cmp = strtoull(a, (char**)&a, 10) -
                                strtoull(b, (char**)&b, 10);
                        } else {
                            cmp = *a - *b;
                            ++a;
                            ++b;
                        }
                    }
                } break;
                case order_mtime:
                    cmp = it->file_time - img->file_time;
                    break;
                case order_size:
                    cmp = it->file_size - img->file_size;
                    break;
                case order_none:
                case order_random:
                    assert(false && "unreachable code");
                    break;
            }
            if ((ctx.reverse && cmp > 0) || (!ctx.reverse && cmp < 0)) {
                pos = it;
                break;
            }
        }
    }

    return pos;
}

/**
 * Add new entry to the list.
 * @param source image data source to add
 * @param st file stat (can be NULL)
 * @return created image entry
 */
static struct image* add_entry(const char* source, const struct stat* st)
{
    struct image* entry;
    struct image* pos;

    // search for duplicates
    entry = imglist_find(source);
    if (entry) {
        return entry;
    }

    // create new entry
    entry = image_create(source);
    if (!entry) {
        return NULL;
    }
    if (st) {
        entry->file_size = st->st_size;
        entry->file_time = st->st_mtime;
    }
    entry->index = ++ctx.size;

    // add entry to the list
    pos = ordered_position(entry);
    if (pos) {
        ctx.images = list_insert(pos, entry);
    } else {
        ctx.images = list_append(ctx.images, entry);
    }

    return entry;
}

/**
 * Add files from the directory to the list.
 * @param dir absolute path to the directory
 * @return the first image entry in the directory
 */
static struct image* add_dir(const char* dir)
{
    struct image* img = NULL;
    struct dirent* dir_entry;
    DIR* dir_handle;

    dir_handle = opendir(dir);
    if (!dir_handle) {
        return NULL;
    }

    while ((dir_entry = readdir(dir_handle))) {
        char path[PATH_MAX] = { 0 };
        const char* name = dir_entry->d_name;
        struct stat st;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue; // skip link to self/parent
        }
        // compose full path
        strncpy(path, dir, sizeof(path) - 1);
        if (!fs_append_path(name, path, sizeof(path))) {
            continue; // buffer too small
        }

        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (ctx.recursive) {
                    fs_append_path(NULL, path, sizeof(path)); // append slash
                    img = add_dir(path);
                }
            } else if (S_ISREG(st.st_mode)) {
                img = add_entry(path, &st);
            }
        }
    }

    // get the first image in the directory
    if (img) {
        const size_t dir_len = strlen(dir);
        list_for_each_back(list_prev(img), struct image, it) {
            if (strncmp(dir, it->source, dir_len) == 0) {
                img = it;
            } else {
                break;
            }
        }
    }

    fs_monitor_add(dir);

    closedir(dir_handle);

    return img;
}

/**
 * Add image source to the list.
 * @param source image source to add (file path or special prefix)
 * @return created image entry or NULL on errors or if source is directory
 */
static struct image* add_source(const char* source)
{
    struct stat st;
    char fspath[PATH_MAX];

    // special url
    if (strncmp(source, LDRSRC_STDIN, LDRSRC_STDIN_LEN) == 0 ||
        strncmp(source, LDRSRC_EXEC, LDRSRC_EXEC_LEN) == 0) {
        return add_entry(source, NULL);
    }

    // file from file system
    if (stat(source, &st) != 0) {
        const int rc = errno;
        fprintf(stderr, "Ignore file %s: [%i] %s\n", source, rc, strerror(rc));
        return NULL;
    }

    // get absolute path
    if (!fs_abspath(source, fspath, sizeof(fspath))) {
        fprintf(stderr, "Ignore file %s: unknown absolute path\n", source);
        return NULL;
    }

    // add directory to the list
    if (S_ISDIR(st.st_mode)) {
        fs_append_path(NULL, fspath, sizeof(fspath)); // append slash
        return add_dir(fspath);
    }

    // add file to the list
    if (S_ISREG(st.st_mode)) {
        struct image* img = add_entry(fspath, &st);
        if (img && !ctx.all_files) {
            fs_monitor_add(img->source);
        }
        return img;
    }

    fprintf(stderr, "Ignore special file %s\n", source);
    return NULL;
}

/**
 * Construct image list from specified sources.
 * @param sources array of sources
 * @param num number of sources in the array
 * @return first image instance to show or NULL if list is empty
 */
static struct image* load_sources(const char* const* sources, size_t num)
{
    struct image* img = NULL;

    // compose image list
    if (num == 0) {
        // no input files specified, use all from the current directory
        img = add_source(".");
        ctx.all_files = false;
    } else if (num == 1) {
        if (strcmp(sources[0], "-") == 0) {
            img = add_source(LDRSRC_STDIN);
        } else {
            if (ctx.all_files) {
                // the "all files" mode is not applicable for directory
                struct stat st;
                if (stat(sources[0], &st) == 0 && S_ISDIR(st.st_mode)) {
                    ctx.all_files = false;
                }
            }
            img = add_source(sources[0]);
            if (img && ctx.all_files) {
                // add neighbors (all files from the same directory)
                const char* delim = strrchr(img->source, '/');
                if (delim) {
                    char dir[PATH_MAX] = { 0 };
                    const size_t len = delim - img->source + 1 /* last slash */;
                    if (len < sizeof(dir)) {
                        strncpy(dir, img->source, len);
                        add_dir(dir);
                    }
                }
            }
        }
    } else {
        ctx.all_files = false;
        for (size_t i = 0; i < num; ++i) {
            struct image* added = add_source(sources[i]);
            if (!img && added) {
                img = added;
            }
        }
    }

    return img;
}

/**
 * Construct image list by loading text lists.
 * @param files array of list files
 * @param num number of sources in the array
 * @return first image instance to show or NULL if list is empty
 */
static struct image* load_fromfile(const char* const* files, size_t num)
{
    ctx.all_files = false; // not applicable in this mode

    for (size_t i = 0; i < num; ++i) {
        char* line = NULL;
        size_t line_sz = 0;
        ssize_t rd;
        FILE* fd;

        fd = fopen(files[i], "r");
        if (!fd) {
            const int rc = errno;
            fprintf(stderr, "Unable to open list file %s: [%i] %s\n", files[i],
                    rc, strerror(rc));
            continue;
        }

        while ((rd = getline(&line, &line_sz, fd)) > 0) {
            while (rd && (line[rd - 1] == '\r' || line[rd - 1] == '\n')) {
                line[--rd] = 0;
            }
            if (*line) {
                add_source(line);
            }
        }

        free(line);
        fclose(fd);
    }

    return imglist_first();
}

/** Reindex the image list. */
static void reindex(void)
{
    ctx.size = 0;
    list_for_each(ctx.images, struct image, it) {
        it->index = ++ctx.size;
    }
}

/** File system event handler. */
static void on_fsevent(enum fsevent type, const char* path)
{
    const size_t path_len = strlen(path);
    const bool is_dir = (path[path_len - 1] == '/'); // ends with '/'

    imglist_lock();

    switch (type) {
        case fsevent_create:
            if (is_dir) {
                if (ctx.recursive) {
                    const struct image* img = add_dir(path);
                    if (img) {
                        app_on_imglist(img, type);
                    }
                }
            } else {
                struct stat st;
                if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                    const struct image* img = add_entry(path, &st);
                    if (img) {
                        app_on_imglist(img, type);
                    }
                }
            }
            break;
        case fsevent_remove:
            if (!is_dir) {
                struct image* img = imglist_find(path);
                if (img) {
                    app_on_imglist(img, type);
                    imglist_remove(img);
                }
            }
            break;
        case fsevent_modify:
            if (!is_dir) {
                const struct image* img = imglist_find(path);
                if (img) {
                    app_on_imglist(img, type);
                }
            }
            break;
    }

    reindex();
    imglist_unlock();
}

/**
 * Get next image with different parent (parent dir).
 * @param img start entry
 * @param loop enable/disable loop mode
 * @param forward direction (forward/backward)
 * @return image instance or NULL if not found
 */
static struct image* get_next_parent(struct image* img, bool loop, bool forward)
{
    const char* cur_src = img->source;
    const char* cur_delim = strrchr(cur_src, '/');
    const size_t cur_len = cur_delim ? cur_delim - cur_src : 0;
    struct image* next = NULL;
    struct image* it = img;

    while (!next) {
        const char* it_src;
        const char* it_delim;
        size_t it_len;

        if (forward) {
            it = list_next(it);
            if (!it && loop) {
                it = ctx.images;
            }
        } else {
            it = list_prev(it);
            if (!it && loop) {
                it = list_get_last(ctx.images);
            }
        }
        if (!it || it == img) {
            break;
        }

        it_src = it->source;
        it_delim = strrchr(it_src, '/');
        it_len = it_delim ? it_delim - it_src : 0;

        if (cur_len != it_len || strncmp(cur_src, it_src, cur_len) != 0) {
            next = it;
        }
    }

    return next;
}

void imglist_init(const struct config* cfg)
{
    pthread_mutex_init(&ctx.lock, NULL);

    ctx.order = config_get_oneof(cfg, CFG_LIST, CFG_LIST_ORDER, order_names,
                                 ARRAY_SIZE(order_names));
    ctx.reverse = config_get_bool(cfg, CFG_LIST, CFG_LIST_REVERSE);
    ctx.loop = config_get_bool(cfg, CFG_LIST, CFG_LIST_LOOP);
    ctx.recursive = config_get_bool(cfg, CFG_LIST, CFG_LIST_RECURSIVE);
    ctx.all_files = config_get_bool(cfg, CFG_LIST, CFG_LIST_ALL);
    ctx.from_file = config_get_bool(cfg, CFG_LIST, CFG_LIST_FROMFILE);

    if (config_get_bool(cfg, CFG_LIST, CFG_LIST_FSMON)) {
        fs_monitor_init(on_fsevent);
    }
}

void imglist_destroy(void)
{
    fs_monitor_destroy();

    list_for_each(ctx.images, struct image, it) {
        image_free(it, IMGFREE_ALL);
    }

    ctx.images = NULL;
    ctx.size = 0;

    pthread_mutex_destroy(&ctx.lock);
}

void imglist_lock(void)
{
    pthread_mutex_lock(&ctx.lock);
}

void imglist_unlock(void)
{
    pthread_mutex_unlock(&ctx.lock);
}

bool imglist_is_locked(void)
{
    if (pthread_mutex_trylock(&ctx.lock) == 0) {
        pthread_mutex_unlock(&ctx.lock);
        return false;
    }
    return true;
}

struct image* imglist_load(const char* const* sources, size_t num)
{
    struct image* img;

    assert(ctx.size == 0 && "already loaded");

    if (ctx.from_file) {
        img = load_fromfile(sources, num);
    } else {
        img = load_sources(sources, num);
    }

    reindex();

    return img;
}

void imglist_remove(struct image* img)
{
    ctx.images = list_remove(img);
    image_free(img, IMGFREE_ALL);
    reindex();
}

struct image* imglist_find(const char* source)
{
    list_for_each(ctx.images, struct image, it) {
        if (strcmp(source, it->source) == 0) {
            return it;
        }
    }
    return NULL;
}

size_t imglist_size(void)
{
    return ctx.size;
}

struct image* imglist_first(void)
{
    return ctx.images;
}

struct image* imglist_last(void)
{
    return list_get_last(ctx.images);
}

struct image* imglist_next(struct image* img)
{
    return list_next(img);
}

struct image* imglist_prev(struct image* img)
{
    return list_prev(img);
}

struct image* imglist_next_file(struct image* img)
{
    struct image* next = imglist_next(img);

    if (!next && ctx.loop) {
        next = ctx.images;
        if (next) {
            if (next == img) {
                next = NULL;
            }
        }
    }

    return next;
}

struct image* imglist_prev_file(struct image* img)
{
    struct image* prev = imglist_prev(img);

    if (!prev && ctx.loop) {
        prev = list_get_last(ctx.images);
        if (prev) {
            if (prev == img) {
                prev = NULL;
            }
        }
    }

    return prev;
}

struct image* imglist_next_dir(struct image* img)
{
    return get_next_parent(img, ctx.loop, true);
}

struct image* imglist_prev_dir(struct image* img)
{
    return get_next_parent(img, ctx.loop, false);
}

struct image* imglist_rand(struct image* img)
{
    size_t offset = 1 + rand() % (ctx.size - 1);

    while (offset--) {
        img = list_next(img);
        if (!img) {
            img = ctx.images;
        }
    }

    return img;
}

struct image* imglist_jump(struct image* img, ssize_t distance)
{
    struct image* target = NULL;

    if (distance > 0) {
        list_for_each(img, struct image, it) {
            if (distance-- == 0) {
                target = it;
                break;
            }
        }
    } else if (distance < 0) {
        list_for_each_back(img, struct image, it) {
            if (distance++ == 0) {
                target = it;
                break;
            }
        }
    } else {
        target = img;
    }

    return target;
}

ssize_t imglist_distance(const struct image* start, const struct image* end)
{
    ssize_t distance = 0;

    if (start->index <= end->index) {
        list_for_each(start, const struct image, it) {
            if (it == end) {
                break;
            }
            ++distance;
        }
    } else {
        list_for_each(end, const struct image, it) {
            if (it == start) {
                break;
            }
            --distance;
        }
    }

    return distance;
}

// SPDX-License-Identifier: MIT
// Gallery mode.
// Copyright (C) 2024 Artem Senichev <artemsen@gmail.com>

#include "gallery.h"

#include "application.h"
#include "array.h"
#include "fs.h"
#include "imglist.h"
#include "info.h"
#include "layout.h"
#include "ui.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Scale for selected thumbnail
#define THUMB_SELECTED_SCALE 1.15f

/** Gallery context. */
struct gallery {
    size_t cache; ///< Max number of thumbnails in cache
    bool preload; ///< Preload invisible thumbnails

    enum aa_mode thumb_aa; ///< Anti-aliasing mode
    bool thumb_fill;       ///< Scale mode (fill/fit)
    bool thumb_pstore;     ///< Use persistent storage for thumbnails

    argb_t clr_window;     ///< Window background
    argb_t clr_background; ///< Tile background
    argb_t clr_select;     ///< Selected tile background
    argb_t clr_border;     ///< Selected tile border
    argb_t clr_shadow;     ///< Selected tile shadow

    struct layout layout; ///< Thumbnail layout

    pthread_t loader_tid; ///< Thumbnail loader thread id
    bool loader_active;   ///< Preload in progress flag
};

/** Global gallery context. */
static struct gallery ctx;

/**
 * Get path for the thumbnail on persistent storage.
 * @param source original image source
 * @param path output buffer
 * @param path_max size of the buffer
 * @return length of the result path without last null
 */
static size_t pstore_path(const char* source, char* path, size_t path_max)
{
    char postfix[16];
    int append_len;
    size_t len;

    if (strcmp(source, LDRSRC_STDIN) == 0 ||
        strncmp(source, LDRSRC_EXEC, LDRSRC_EXEC_LEN) == 0) {
        return 0;
    }

    // get directory to store thumbnails
    len = fs_envpath("XDG_CACHE_HOME", "/swayimg", path, path_max);
    if (!len) {
        len = fs_envpath("HOME", "/.cache/swayimg", path, path_max);
    }
    if (!len) {
        return 0;
    }

    // append file name
    append_len = fs_append_path(source, path, path_max);
    if (!append_len) {
        return 0;
    }
    len += append_len;

    // append postfix
    append_len = snprintf(postfix, sizeof(postfix), ".%04x%d%d",
                          (uint16_t)ctx.layout.thumb_size,
                          ctx.thumb_fill ? 1 : 0, ctx.thumb_aa);
    if (append_len <= 0 || len + append_len + 1 >= path_max) {
        return 0;
    }
    memcpy(path + len, postfix, append_len + 1);
    len += append_len;

    return len;
}

/**
 * Save thumbnail on persistent storage.
 * @param img image with thumbnail to save
 */
void pstore_save(const struct image* img)
{
    char path[PATH_MAX];
    char* delim;

    if (!image_has_thumb(img)) {
        return;
    }

    if (!pstore_path(img->source, path, sizeof(path))) {
        return;
    }

    // create path
    delim = path;
    while (true) {
        delim = strchr(delim + 1, '/');
        if (!delim) {
            break;
        }
        *delim = '\0';
        if (mkdir(path, S_IRWXU | S_IRWXG) && errno != EEXIST) {
            return;
        }
        *delim = '/';
    }

    image_thumb_save(img, path);
}

/**
 * Load thumbnail from persistent storage.
 * @param img image for loading thumbnail
 * @return true if thumbnail loaded
 */
bool pstore_load(struct image* img)
{
    char path[PATH_MAX];
    struct stat st_image;
    struct stat st_thumb;

    if (!pstore_path(img->source, path, sizeof(path))) {
        return false;
    }

    // check modification time
    if (stat(img->source, &st_image) == -1 || stat(path, &st_thumb) == -1 ||
        st_image.st_mtim.tv_sec > st_thumb.st_mtim.tv_sec) {
        return false;
    }

    return image_thumb_load(img, path);
}

/**
 * Remove non-visible thumbnails to save memory.
 * @param all remove even visible thumbnails
 */
static void clear_thumbnails(bool all)
{
    imglist_lock();

    if (all) {
        struct image* img = imglist_first();
        while (img) {
            image_free(img, IMGFREE_THUMB);
            img = imglist_next(img);
        }
    } else if (ctx.cache) {
        layout_update(&ctx.layout);
        layout_clear(&ctx.layout, ctx.cache);
    }

    imglist_unlock();
}

/**
 * Thumbnail loader thread.
 */
static void* loader_thread(void* data)
{
    struct image* queue = data;

    list_for_each(queue, struct image, it) {
        struct image* origin;

        // check if thumbnail is already loaded
        imglist_lock();
        if (!ctx.loader_active) {
            imglist_unlock();
            break;
        }
        origin = imglist_find(it->source);
        if (origin) {
            if (image_has_thumb(origin)) {
                origin = NULL; // already loaded
            }
            if (image_thumb_create(origin, ctx.layout.thumb_size,
                                   ctx.thumb_fill, ctx.thumb_aa)) {
                app_redraw();
                origin = NULL; // loaded from frame data
            }
        }
        imglist_unlock();

        if (!origin) {
            continue;
        }

        // load thumbnail
        if (!ctx.thumb_pstore || !pstore_load(it)) {
            if (image_load(it) == imgload_success) {
                if (image_thumb_create(it, ctx.layout.thumb_size,
                                       ctx.thumb_fill, ctx.thumb_aa) &&
                    ctx.thumb_pstore) {
                    // save to thumbnail to persistent storage
                    const size_t width = it->frames[0].pm.width;
                    const size_t height = it->frames[0].pm.height;
                    if (width > ctx.layout.thumb_size &&
                        height > ctx.layout.thumb_size) {
                        pstore_save(it);
                    }
                }
                image_free(it, IMGFREE_FRAMES); // not needed anymore
            }
        }

        // put thumbnail to image list
        imglist_lock();
        if (!ctx.loader_active) {
            imglist_unlock();
            break;
        }
        origin = imglist_find(it->source);
        if (origin) {
            if (image_has_thumb(it)) {
                image_update(origin, it);
            } else {
                imglist_remove(origin); // failed to load
            }
        }
        imglist_unlock();

        app_redraw();
    }

    // free the queue
    list_for_each(queue, struct image, it) {
        image_free(it, IMGFREE_ALL);
    }
    if (ctx.loader_active) {
        clear_thumbnails(false);
    }

    ctx.loader_active = false;
    return NULL;
}

/**
 * Start thumbnail loader thread.
 * @param queue head of image list to load
 */
static void loader_restart(struct image* queue)
{
    if (ctx.loader_active) {
        ctx.loader_active = false;
        pthread_join(ctx.loader_tid, NULL);
    }
    if (queue) {
        ctx.loader_active = true;
        pthread_create(&ctx.loader_tid, NULL, loader_thread, queue);
    }
}

/**
 * Select next file.
 * @param direction next image position in list
 * @return true if next image was selected
 */
static bool select_next(enum action_type direction)
{
    bool rc = false;
    struct image* load = NULL;
    enum layout_dir dir;

    switch (direction) {
        case action_first_file:
            dir = layout_first;
            break;
        case action_last_file:
            dir = layout_last;
            break;
        case action_prev_file:
        case action_step_left:
            dir = layout_left;
            break;
        case action_next_file:
        case action_step_right:
            dir = layout_right;
            break;
        case action_step_up:
            dir = layout_up;
            break;
        case action_step_down:
            dir = layout_down;
            break;
        case action_page_up:
            dir = layout_pgup;
            break;
        case action_page_down:
            dir = layout_pgdown;
            break;
        default:
            assert(false && "unreachable code");
            return false;
    }

    imglist_lock();
    rc = layout_select(&ctx.layout, dir);
    if (rc) {
        load = layout_ldqueue(&ctx.layout, ctx.cache);
    }
    imglist_unlock();

    if (rc) {
        if (load) {
            loader_restart(load);
        }
        info_reset(ctx.layout.current);
        app_redraw();
    }

    return rc;
}

/**
 * Skip current image file.
 * @param remove flag to remove current image from the image list
 */
static void skip_current(bool remove)
{
    struct image* skip = ctx.layout.current;

    if (layout_select(&ctx.layout, layout_right) ||
        layout_select(&ctx.layout, layout_left)) {
        if (remove) {
            imglist_remove(skip);
        }
        app_redraw();
    } else {
        printf("No more images to view, exit\n");
        app_exit(0);
    }
}

/** Reload. */
static void reload(void)
{
    loader_restart(NULL);
    clear_thumbnails(true);
    app_redraw();
}

/**
 * Draw thumbnail.
 * @param window destination window
 * @param thumb thumbnail description
 */
static void draw_thumbnail(struct pixmap* window,
                           const struct layout_thumb* lth)
{
    const struct pixmap* pm =
        image_has_thumb(lth->img) ? &lth->img->thumbnail : NULL;
    ssize_t x = lth->x;
    ssize_t y = lth->y;

    if (lth != layout_current(&ctx.layout)) {
        pixmap_fill(window, x, y, ctx.layout.thumb_size, ctx.layout.thumb_size,
                    ctx.clr_background);
        if (pm) {
            x += ctx.layout.thumb_size / 2 - pm->width / 2;
            y += ctx.layout.thumb_size / 2 - pm->height / 2;
            pixmap_copy(pm, window, x, y, lth->img->alpha);
        }
    } else {
        // currently selected item
        const size_t thumb_size = THUMB_SELECTED_SCALE * ctx.layout.thumb_size;
        const ssize_t thumb_offset = (thumb_size - ctx.layout.thumb_size) / 2;

        x = max(0, x - thumb_offset);
        y = max(0, y - thumb_offset);
        if (x + thumb_size >= window->width) {
            x = window->width - thumb_size;
        }

        pixmap_fill(window, x, y, thumb_size, thumb_size, ctx.clr_select);

        if (pm) {
            const size_t thumb_w = pm->width * THUMB_SELECTED_SCALE;
            const size_t thumb_h = pm->height * THUMB_SELECTED_SCALE;
            const ssize_t tx = x + thumb_size / 2 - thumb_w / 2;
            const ssize_t ty = y + thumb_size / 2 - thumb_h / 2;
            pixmap_scale(ctx.thumb_aa, pm, window, tx, ty, THUMB_SELECTED_SCALE,
                         lth->img->alpha);
        }

        // shadow
        if (ARGB_GET_A(ctx.clr_shadow)) {
            const argb_t base = ctx.clr_shadow & 0x00ffffff;
            const uint8_t alpha = ARGB_GET_A(ctx.clr_shadow);
            const size_t width =
                max(1, (double)thumb_size / 15.0 * ((double)alpha / 255.0));
            const size_t alpha_step = alpha / width;

            for (size_t i = 0; i < width; ++i) {
                const ssize_t lx = i + x + thumb_size;
                const ssize_t ly = y + width;
                const size_t lh = thumb_size - (width - i);
                const argb_t color = base | ARGB_SET_A(alpha - i * alpha_step);
                pixmap_vline(window, lx, ly, lh, color);
            }
            for (size_t i = 0; i < width; ++i) {
                const ssize_t lx = x + width;
                const ssize_t ly = y + thumb_size + i;
                const size_t lw = thumb_size - (width - i) + 1;
                const argb_t color = base | ARGB_SET_A(alpha - i * alpha_step);
                pixmap_hline(window, lx, ly, lw, color);
            }
        }

        // border
        if (ARGB_GET_A(ctx.clr_border)) {
            pixmap_rect(window, x, y, thumb_size, thumb_size, ctx.clr_border);
        }
    }
}

/**
 * Draw thumbnails.
 * @param window destination window
 */
static void draw_thumbnails(struct pixmap* window)
{
    struct image* load = NULL;
    bool all_loaded = true;

    imglist_lock();
    layout_update(&ctx.layout);

    // draw all exclude the currently selected
    for (size_t i = 0; i < ctx.layout.thumb_total; ++i) {
        const struct layout_thumb* thumb = &ctx.layout.thumbs[i];
        all_loaded &= image_has_thumb(thumb->img);
        if (thumb != layout_current(&ctx.layout)) {
            draw_thumbnail(window, thumb);
        }
    }
    // draw only currently selected
    draw_thumbnail(window, layout_current(&ctx.layout));

    if (!all_loaded && !ctx.loader_active) {
        load = layout_ldqueue(&ctx.layout, ctx.cache);
    }

    imglist_unlock();

    if (load) {
        loader_restart(load);
    }
}

/** Mode handler: window redraw. */
static void on_redraw(struct pixmap* window)
{
    pixmap_fill(window, 0, 0, window->width, window->height, ctx.clr_window);
    draw_thumbnails(window);
    info_print(window);
}

/** Mode handler: window resize. */
static void on_resize(void)
{
    loader_restart(NULL);

    imglist_lock();
    layout_resize(&ctx.layout, ui_get_width(), ui_get_height());
    imglist_unlock();
}

/** Mode handler: apply action. */
static void on_action(const struct action* action)
{
    switch (action->type) {
        case action_antialiasing:
            ctx.thumb_aa = aa_switch(ctx.thumb_aa, action->params);
            info_update(info_status, "Anti-aliasing: %s",
                        aa_name(ctx.thumb_aa));
            reload();
            break;
        case action_first_file:
        case action_last_file:
        case action_prev_file:
        case action_next_file:
        case action_step_left:
        case action_step_right:
        case action_step_up:
        case action_step_down:
        case action_page_up:
        case action_page_down:
            select_next(action->type);
            break;
        case action_skip_file:
            imglist_lock();
            skip_current(true);
            imglist_unlock();
            break;
        case action_reload:
            reload();
            break;
        default:
            break;
    }
}

/** Mode handler: image list update. */
static void on_imglist(const struct image* image, enum fsevent event)
{
    switch (event) {
        case fsevent_create:
            break;
        case fsevent_modify:
            if (image == ctx.layout.current) {
                reload();
            }
            break;
        case fsevent_remove:
            if (image == ctx.layout.current) {
                skip_current(false);
            }
            break;
    }
    app_redraw();
}

/** Mode handler: get currently viewed image. */
static struct image* on_current(void)
{
    return ctx.layout.current;
}

/** Mode handler: activate viewer. */
static void on_activate(struct image* image)
{
    imglist_lock();

    ctx.layout.current = image;
    layout_resize(&ctx.layout, ui_get_width(), ui_get_height());

    if (!image_has_thumb(image)) {
        image_thumb_create(image, ctx.layout.thumb_size, ctx.thumb_fill,
                           ctx.thumb_aa);
    }

    imglist_unlock();

    info_reset(image);
}

/** Mode handler: deactivate viewer. */
static struct image* on_deactivate(void)
{
    loader_restart(NULL);
    return ctx.layout.current;
}

void gallery_init(const struct config* cfg, struct mode_handlers* handlers)
{
    const size_t ts = config_get_num(cfg, CFG_GALLERY, CFG_GLRY_SIZE, 1, 4096);
    layout_init(&ctx.layout, ts);

    ctx.cache = config_get_num(cfg, CFG_GALLERY, CFG_GLRY_CACHE, 0, SSIZE_MAX);
    ctx.preload = config_get_bool(cfg, CFG_GALLERY, CFG_GLRY_PRELOAD);

    ctx.thumb_aa = aa_init(cfg, CFG_GALLERY, CFG_GLRY_AA);
    ctx.thumb_fill = config_get_bool(cfg, CFG_GALLERY, CFG_GLRY_FILL);
    ctx.thumb_pstore = config_get_bool(cfg, CFG_GALLERY, CFG_GLRY_PSTORE);

    ctx.clr_window = config_get_color(cfg, CFG_GALLERY, CFG_GLRY_WINDOW);
    ctx.clr_background = config_get_color(cfg, CFG_GALLERY, CFG_GLRY_BKG);
    ctx.clr_select = config_get_color(cfg, CFG_GALLERY, CFG_GLRY_SELECT);
    ctx.clr_border = config_get_color(cfg, CFG_GALLERY, CFG_GLRY_BORDER);
    ctx.clr_shadow = config_get_color(cfg, CFG_GALLERY, CFG_GLRY_SHADOW);

    handlers->action = on_action;
    handlers->redraw = on_redraw;
    handlers->resize = on_resize;
    handlers->imglist = on_imglist;
    handlers->current = on_current;
    handlers->activate = on_activate;
    handlers->deactivate = on_deactivate;
}

void gallery_destroy(void)
{
    loader_restart(NULL);
}

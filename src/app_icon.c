/* ========================================================================= */
/**
 * @file app_icon.c
 *
 * @copyright
 * Copyright 2026 Petr Demuth
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "app_icon.h"

#include <dirent.h>
#include <librsvg/rsvg.h>
#include <libbase/libbase.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* == Declarations ========================================================= */

/** Maximum number of XDG data directories considered. */
#define _APP_ICON_MAX_DIRS 16

/** List of XDG data directories, most specific first. */
typedef struct {
    /** The directories. */
    char                      *dirs[_APP_ICON_MAX_DIRS];
    /** Number of entries in `dirs`. */
    size_t                    n;
} _app_icon_dirs_t;

static void _app_icon_dirs_init(_app_icon_dirs_t *dirs_ptr);
static void _app_icon_dirs_fini(_app_icon_dirs_t *dirs_ptr);
static void _app_icon_dirs_add(_app_icon_dirs_t *dirs_ptr, const char *dir_ptr);
static bool _app_icon_is_file(const char *path_ptr);
static char *_app_icon_desktop_key(const char *path_ptr, const char *key_ptr);
static char *_app_icon_find_desktop_entry(
    const _app_icon_dirs_t *dirs_ptr,
    const char *app_id_ptr);
static char *_app_icon_theme_name(void);
static char *_app_icon_resolve_name(
    const _app_icon_dirs_t *dirs_ptr,
    const char *icon_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
char *wlmaker_app_icon_find(const char *app_id_ptr)
{
    if (NULL == app_id_ptr || 0 == *app_id_ptr) return NULL;

    _app_icon_dirs_t dirs;
    _app_icon_dirs_init(&dirs);

    char *icon_ptr = NULL;
    char *desktop_path_ptr = _app_icon_find_desktop_entry(&dirs, app_id_ptr);
    if (NULL != desktop_path_ptr) {
        icon_ptr = _app_icon_desktop_key(desktop_path_ptr, "Icon");
        free(desktop_path_ptr);
    }

    // Without a desktop entry, the app ID may still name an icon.
    char *path_ptr = _app_icon_resolve_name(
        &dirs, NULL != icon_ptr ? icon_ptr : app_id_ptr);
    if (NULL == path_ptr && NULL != icon_ptr) {
        path_ptr = _app_icon_resolve_name(&dirs, app_id_ptr);
    }
    free(icon_ptr);
    _app_icon_dirs_fini(&dirs);
    return path_ptr;
}

/* ------------------------------------------------------------------------- */
cairo_surface_t *wlmaker_app_icon_load(const char *path_ptr, int size)
{
    cairo_surface_t *surface_ptr = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, size, size);
    if (CAIRO_STATUS_SUCCESS != cairo_surface_status(surface_ptr)) {
        cairo_surface_destroy(surface_ptr);
        return NULL;
    }
    cairo_t *cairo_ptr = cairo_create(surface_ptr);
    bool ok = false;

    size_t len = strlen(path_ptr);
    if (4 < len && 0 == strcasecmp(path_ptr + len - 4, ".svg")) {
        GError *error_ptr = NULL;
        RsvgHandle *handle_ptr = rsvg_handle_new_from_file(
            path_ptr, &error_ptr);
        if (NULL != handle_ptr) {
            RsvgRectangle viewport = { 0, 0, size, size };
            ok = rsvg_handle_render_document(
                handle_ptr, cairo_ptr, &viewport, &error_ptr);
            g_object_unref(handle_ptr);
        }
        if (NULL != error_ptr) {
            bs_log(BS_WARNING, "Failed to render icon \"%s\": %s",
                   path_ptr, error_ptr->message);
            g_error_free(error_ptr);
        }
    } else {
        cairo_surface_t *png_ptr = cairo_image_surface_create_from_png(
            path_ptr);
        if (CAIRO_STATUS_SUCCESS == cairo_surface_status(png_ptr)) {
            int w = cairo_image_surface_get_width(png_ptr);
            int h = cairo_image_surface_get_height(png_ptr);
            double scale = (double)size / (double)BS_MAX(w, h);
            cairo_translate(cairo_ptr,
                            (size - w * scale) / 2.0, (size - h * scale) / 2.0);
            cairo_scale(cairo_ptr, scale, scale);
            cairo_set_source_surface(cairo_ptr, png_ptr, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cairo_ptr),
                                     CAIRO_FILTER_GOOD);
            cairo_paint(cairo_ptr);
            ok = true;
        } else {
            bs_log(BS_WARNING, "Failed to load icon \"%s\"", path_ptr);
        }
        cairo_surface_destroy(png_ptr);
    }

    cairo_destroy(cairo_ptr);
    if (!ok) {
        cairo_surface_destroy(surface_ptr);
        return NULL;
    }
    cairo_surface_flush(surface_ptr);
    return surface_ptr;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Collects XDG data directories: data home, data dirs, Flatpak exports. */
void _app_icon_dirs_init(_app_icon_dirs_t *dirs_ptr)
{
    *dirs_ptr = (_app_icon_dirs_t){};
    const char *home_ptr = getenv("HOME");

    const char *data_home_ptr = getenv("XDG_DATA_HOME");
    if (NULL != data_home_ptr && *data_home_ptr) {
        _app_icon_dirs_add(dirs_ptr, data_home_ptr);
    } else if (NULL != home_ptr) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s/.local/share", home_ptr);
        _app_icon_dirs_add(dirs_ptr, buf);
    }

    const char *data_dirs_ptr = getenv("XDG_DATA_DIRS");
    if (NULL == data_dirs_ptr || 0 == *data_dirs_ptr) {
        data_dirs_ptr = "/usr/local/share:/usr/share";
    }
    char *copy_ptr = logged_strdup(data_dirs_ptr);
    if (NULL != copy_ptr) {
        char *save_ptr = NULL;
        for (char *dir_ptr = strtok_r(copy_ptr, ":", &save_ptr);
             NULL != dir_ptr;
             dir_ptr = strtok_r(NULL, ":", &save_ptr)) {
            _app_icon_dirs_add(dirs_ptr, dir_ptr);
        }
        free(copy_ptr);
    }

    if (NULL != home_ptr) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s/.local/share/flatpak/exports/share",
                 home_ptr);
        _app_icon_dirs_add(dirs_ptr, buf);
    }
    _app_icon_dirs_add(dirs_ptr, "/var/lib/flatpak/exports/share");
}

/* ------------------------------------------------------------------------- */
/** Releases the directory list. */
void _app_icon_dirs_fini(_app_icon_dirs_t *dirs_ptr)
{
    for (size_t i = 0; i < dirs_ptr->n; ++i) free(dirs_ptr->dirs[i]);
    dirs_ptr->n = 0;
}

/* ------------------------------------------------------------------------- */
/** Adds `dir_ptr`, unless already listed or the list is full. */
void _app_icon_dirs_add(_app_icon_dirs_t *dirs_ptr, const char *dir_ptr)
{
    if (_APP_ICON_MAX_DIRS <= dirs_ptr->n) return;
    for (size_t i = 0; i < dirs_ptr->n; ++i) {
        if (0 == strcmp(dirs_ptr->dirs[i], dir_ptr)) return;
    }
    char *copy_ptr = logged_strdup(dir_ptr);
    if (NULL != copy_ptr) dirs_ptr->dirs[dirs_ptr->n++] = copy_ptr;
}

/* ------------------------------------------------------------------------- */
/** @return whether `path_ptr` is a regular file (following links). */
bool _app_icon_is_file(const char *path_ptr)
{
    struct stat st;
    return 0 == stat(path_ptr, &st) && S_ISREG(st.st_mode);
}

/* ------------------------------------------------------------------------- */
/** @return The value of `key_ptr` in the [Desktop Entry] group, or NULL. */
char *_app_icon_desktop_key(const char *path_ptr, const char *key_ptr)
{
    FILE *file_ptr = fopen(path_ptr, "r");
    if (NULL == file_ptr) return NULL;

    char line[1024];
    bool in_entry = false;
    size_t key_len = strlen(key_ptr);
    char *value_ptr = NULL;
    while (NULL == value_ptr && NULL != fgets(line, sizeof(line), file_ptr)) {
        line[strcspn(line, "\r\n")] = '\0';
        if ('[' == line[0]) {
            in_entry = (0 == strcmp(line, "[Desktop Entry]"));
            continue;
        }
        if (!in_entry || 0 != strncmp(line, key_ptr, key_len)) continue;
        const char *p = line + key_len;
        while (' ' == *p) ++p;
        if ('=' != *p) continue;  // eg. "Icon[cs]=" or "IconFoo=".
        ++p;
        while (' ' == *p) ++p;
        value_ptr = logged_strdup(p);
    }
    fclose(file_ptr);
    return value_ptr;
}

/* ------------------------------------------------------------------------- */
/**
 * Finds the desktop entry for the app ID: `<app_id>.desktop` (also in lower
 * case), else an entry whose StartupWMClass or name matches, ignoring case.
 *
 * @return Path of the desktop entry, to be released with free(), or NULL.
 */
char *_app_icon_find_desktop_entry(
    const _app_icon_dirs_t *dirs_ptr,
    const char *app_id_ptr)
{
    char path[1024];
    for (size_t i = 0; i < dirs_ptr->n; ++i) {
        snprintf(path, sizeof(path), "%s/applications/%s.desktop",
                 dirs_ptr->dirs[i], app_id_ptr);
        if (_app_icon_is_file(path)) return logged_strdup(path);
    }

    for (size_t i = 0; i < dirs_ptr->n; ++i) {
        snprintf(path, sizeof(path), "%s/applications", dirs_ptr->dirs[i]);
        DIR *dir_ptr = opendir(path);
        if (NULL == dir_ptr) continue;

        char *found_ptr = NULL;
        struct dirent *dirent_ptr;
        while (NULL == found_ptr && NULL != (dirent_ptr = readdir(dir_ptr))) {
            const char *name_ptr = dirent_ptr->d_name;
            size_t len = strlen(name_ptr);
            if (len <= 8 || 0 != strcmp(name_ptr + len - 8, ".desktop")) {
                continue;
            }
            char entry_path[1280];
            snprintf(entry_path, sizeof(entry_path), "%s/%s", path, name_ptr);

            // Name match, ignoring case: "Kate" -> org.kde.kate.desktop.
            size_t id_len = strlen(app_id_ptr);
            const char *base_ptr = name_ptr;
            const char *dot_ptr = NULL;
            for (const char *p = name_ptr; p < name_ptr + len - 8; ++p) {
                if ('.' == *p) dot_ptr = p;
            }
            if (NULL != dot_ptr) base_ptr = dot_ptr + 1;
            if ((len - 8 == id_len &&
                 0 == strncasecmp(name_ptr, app_id_ptr, id_len)) ||
                ((size_t)(name_ptr + len - 8 - base_ptr) == id_len &&
                 0 == strncasecmp(base_ptr, app_id_ptr, id_len))) {
                found_ptr = logged_strdup(entry_path);
                break;
            }

            char *wm_class_ptr = _app_icon_desktop_key(
                entry_path, "StartupWMClass");
            if (NULL != wm_class_ptr) {
                if (0 == strcasecmp(wm_class_ptr, app_id_ptr)) {
                    found_ptr = logged_strdup(entry_path);
                }
                free(wm_class_ptr);
            }
        }
        closedir(dir_ptr);
        if (NULL != found_ptr) return found_ptr;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/** @return The icon theme from kdeglobals' [Icons] Theme, or "breeze". */
char *_app_icon_theme_name(void)
{
    const char *home_ptr = getenv("HOME");
    if (NULL != home_ptr) {
        char path[512];
        snprintf(path, sizeof(path), "%s/.config/kdeglobals", home_ptr);
        FILE *file_ptr = fopen(path, "r");
        if (NULL != file_ptr) {
            char line[512];
            bool in_icons = false;
            while (NULL != fgets(line, sizeof(line), file_ptr)) {
                line[strcspn(line, "\r\n")] = '\0';
                if ('[' == line[0]) {
                    in_icons = (0 == strcmp(line, "[Icons]"));
                } else if (in_icons && 0 == strncmp(line, "Theme=", 6)) {
                    fclose(file_ptr);
                    return logged_strdup(line + 6);
                }
            }
            fclose(file_ptr);
        }
    }
    return logged_strdup("breeze");
}

/* ------------------------------------------------------------------------- */
/**
 * Resolves an `Icon` value: An absolute path, or a name looked up in the
 * icon themes (largest sizes and SVG first) and in pixmaps.
 *
 * @return Path to the icon file, to be released with free(), or NULL.
 */
char *_app_icon_resolve_name(
    const _app_icon_dirs_t *dirs_ptr,
    const char *icon_ptr)
{
    if ('/' == icon_ptr[0]) {
        return _app_icon_is_file(icon_ptr) ? logged_strdup(icon_ptr) : NULL;
    }

    // Layouts: freedesktop ("48x48/apps") and Breeze ("apps/48").
    static const char *subdirs[] = {
        "scalable/apps", "apps/64", "apps/48", "512x512/apps", "256x256/apps",
        "128x128/apps", "96x96/apps", "64x64/apps", "48x48/apps",
        "32x32/apps", NULL
    };
    static const char *exts[] = { "svg", "png", NULL };

    char *theme_ptr = _app_icon_theme_name();
    const char *themes[] = { theme_ptr, "breeze", "hicolor", NULL };
    char path[1024];

    for (const char **t = themes; NULL != *t; ++t) {
        if (t != themes && NULL != theme_ptr && 0 == strcmp(*t, theme_ptr)) {
            continue;
        }
        for (size_t i = 0; i < dirs_ptr->n; ++i) {
            for (const char **s = subdirs; NULL != *s; ++s) {
                for (const char **e = exts; NULL != *e; ++e) {
                    snprintf(path, sizeof(path), "%s/icons/%s/%s/%s.%s",
                             dirs_ptr->dirs[i], *t, *s, icon_ptr, *e);
                    if (_app_icon_is_file(path)) {
                        free(theme_ptr);
                        return logged_strdup(path);
                    }
                }
            }
        }
    }
    free(theme_ptr);

    for (size_t i = 0; i < dirs_ptr->n; ++i) {
        for (const char **e = exts; NULL != *e; ++e) {
            snprintf(path, sizeof(path), "%s/pixmaps/%s.%s",
                     dirs_ptr->dirs[i], icon_ptr, *e);
            if (_app_icon_is_file(path)) return logged_strdup(path);
        }
        // `Icon` may carry the extension already.
        snprintf(path, sizeof(path), "%s/pixmaps/%s", dirs_ptr->dirs[i],
                 icon_ptr);
        if (_app_icon_is_file(path)) return logged_strdup(path);
    }
    return NULL;
}

/* == End of app_icon.c ==================================================== */

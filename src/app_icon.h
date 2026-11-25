/* ========================================================================= */
/**
 * @file app_icon.h
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
#ifndef __APP_ICON_H__
#define __APP_ICON_H__

#include <cairo.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Finds the icon file of an application, as desktop menus do: Looks up the
 * XDG desktop entry for `app_id_ptr` (by file name, or StartupWMClass),
 * reads its `Icon`, and resolves that through the icon theme (the KDE theme
 * from kdeglobals, breeze, hicolor) and pixmaps. Includes Flatpak exports.
 *
 * @param app_id_ptr          XDG app_id or X11 WM_CLASS class.
 *
 * @return Path to a PNG or SVG file, to be released with free(), or NULL.
 */
char *wlmaker_app_icon_find(const char *app_id_ptr);

/**
 * Loads a PNG or SVG icon, scaled to fit `size` x `size` pixels.
 *
 * @param path_ptr
 * @param size
 *
 * @return An ARGB32 image surface, to be destroyed with
 *     cairo_surface_destroy(), or NULL on error.
 */
cairo_surface_t *wlmaker_app_icon_load(const char *path_ptr, int size);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __APP_ICON_H__ */
/* == End of app_icon.h ==================================================== */

/* ========================================================================= */
/**
 * @file dock.h
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2023 Google LLC
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
 *
 * Creates the wlmaker dock: A view, with server-bound surfaces, that acts as
 * launcher for Apps.
 *
 * Corresponding Window Maker documentation:
 * http://www.windowmaker.org/docs/guidedtour/dock.html
 */
#ifndef __DOCK_H__
#define __DOCK_H__

#include <libbase/libbase.h>
#include <libbase/plist.h>

/** Forward definition: Dock handle. */
typedef struct _wlmaker_dock_t wlmaker_dock_t;

#include "config.h"
#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Creates the Dock handle. Needs the server to be up with workspaces running.
 *
 * @param server_ptr
 * @param state_dict_ptr
 * @param style_ptr
 *
 * @return Pointer to the Dock handle, or NULL on error.
 */
wlmaker_dock_t *wlmaker_dock_create(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *state_dict_ptr,
    const wlmaker_config_style_t *style_ptr);

/** Layout of a pane (dock, clip, icon area), from its dict in the state. */
typedef struct {
    /** `Lines`: Number of lines (rows or columns) of tiles, 1..3. */
    int64_t                   lines;
    /** `Order`: Every other line runs back (Snake), or all alike (Straight). */
    bool                      snake;
    /** `TileSize`: Size of the pane's tiles; 0 for the theme's. */
    uint64_t                  tile_size;
} wlmaker_pane_layout_t;

/**
 * Decodes `Lines`, `Order` and `TileSize` of a pane's state dict. Missing or
 * invalid values keep their defaults: 1 line, Snake, the theme's tile size.
 *
 * @param dict_ptr            May be NULL.
 * @param layout_ptr
 */
void wlmaker_pane_layout_decode(
    bspl_dict_t *dict_ptr,
    wlmaker_pane_layout_t *layout_ptr);

/**
 * @return The theme's tile style, scaled to `tile_size` (0: unchanged).
 */
struct wlmtk_tile_style wlmaker_pane_tile_style(
    uint64_t tile_size,
    const struct wlmtk_tile_style *theme_tile_style_ptr);

/**
 * Destroys the Dock handle.
 *
 * @param dock_ptr
 */
void wlmaker_dock_destroy(wlmaker_dock_t *dock_ptr);

/** Unit test test. */
extern const bs_test_set_t wlmaker_dock_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __DOCK_H__ */
/* == End of dock.h ======================================================== */

/* ========================================================================= */
/**
 * @file background.h
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2024 Google LLC
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
#ifndef __BACKGROUND_H__
#define __BACKGROUND_H__

#include <libbase/libbase.h>
#include <stdint.h>

#include "toolkit/toolkit.h"

struct wlr_output_layout;

/** How a wallpaper image is fit to an output. */
typedef enum {
    WLMAKER_BACKGROUND_FILL,     ///< Scaled to cover the output, cropped.
    WLMAKER_BACKGROUND_SCALE,    ///< Scaled to fit inside, color borders.
    WLMAKER_BACKGROUND_STRETCH,  ///< Stretched to the output's size.
    WLMAKER_BACKGROUND_CENTER,   ///< Unscaled, centered.
    WLMAKER_BACKGROUND_TILE,     ///< Unscaled, repeated from the top left.
} wlmaker_background_mode_t;

/** Names of @ref wlmaker_background_mode_t, for the "ImageMode" key. */
extern const bspl_enum_desc_t wlmaker_background_mode_desc[];

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Creates a background, derived from a @ref wlmtk_panel_t.
 *
 * @param workspace_ptr
 * @param wlr_output_layout_ptr
 * @param color
 *
 * @return A list node acting as handle for the background, or NULL on error.
 */
bs_dllist_node_t *wlmaker_background_create(
    wlmtk_workspace_t *workspace_ptr,
    struct wlr_output_layout *wlr_output_layout_ptr,
    uint32_t color);

/**
 * Destroys the background.
 *
 * @param dlnode_ptr
 * @param ud_ptr
 */
void wlmaker_background_dlnode_destroy(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);

/**
 * Sets the color for this workspace's background on each output.
 *
 * @param dlnode_ptr
 * @param ud_ptr
 */
void wlmaker_background_dlnode_set_color(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);

/**
 * Sets (or clears) the wallpaper image of this workspace's background.
 *
 * @param dlnode_ptr
 * @param path_ptr            Path to a PNG file. NULL or empty: No image.
 * @param mode
 */
void wlmaker_background_dlnode_set_image(
    bs_dllist_node_t *dlnode_ptr,
    const char *path_ptr,
    wlmaker_background_mode_t mode);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __BACKGROUND_H__ */
/* == End of background.h ================================================== */

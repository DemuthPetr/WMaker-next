/* ========================================================================= */
/**
 * @file icon_area.h
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
#ifndef __ICON_AREA_H__
#define __ICON_AREA_H__

/** Forward declaration: Icon area handle. */
typedef struct _wlmaker_icon_area_t wlmaker_icon_area_t;

#include "config.h"
#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Creates the icon area: The row of mini-windows for minimized windows,
 * along the bottom-left of the primary output (as in Window Maker).
 *
 * Shows the mini-windows of windows on the current workspace. Double-
 * clicking a mini-window restores its window.
 *
 * @param server_ptr
 * @param style_ptr
 *
 * @return Pointer to the icon area, or NULL on error. Must be destroyed by
 *     calling @ref wlmaker_icon_area_destroy.
 */
wlmaker_icon_area_t *wlmaker_icon_area_create(
    wlmaker_server_t *server_ptr,
    const wlmaker_config_style_t *style_ptr);

/**
 * Applies the state's optional `IconArea` positioning (`Edge`, `Anchor`;
 * default: bottom edge, from the left): Moves the icon area.
 *
 * @param icon_area_ptr
 * @param state_dict_ptr
 *
 * @return true on success.
 */
bool wlmaker_icon_area_set_state(
    wlmaker_icon_area_t *icon_area_ptr,
    bspl_dict_t *state_dict_ptr);

/**
 * Destroys the icon area.
 *
 * @param icon_area_ptr
 */
void wlmaker_icon_area_destroy(wlmaker_icon_area_t *icon_area_ptr);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __ICON_AREA_H__ */
/* == End of icon_area.h =================================================== */

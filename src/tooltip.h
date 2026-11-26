/* ========================================================================= */
/**
 * @file tooltip.h
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
#ifndef __TOOLTIP_H__
#define __TOOLTIP_H__

#include "toolkit/toolkit.h"

struct wl_event_loop;
struct wlr_output_layout;
struct wlr_scene_tree;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Sets up tooltips (Window Maker's balloon help): A small box with a text,
 * shown beside an element after the pointer rested on it for a moment.
 *
 * @param wl_event_loop_ptr
 * @param wlr_output_layout_ptr
 * @param wlr_scene_tree_ptr  Tree to show the tooltip in, atop everything.
 *
 * @return true on success.
 */
bool wlmaker_tooltip_init(
    struct wl_event_loop *wl_event_loop_ptr,
    struct wlr_output_layout *wlr_output_layout_ptr,
    struct wlr_scene_tree *wlr_scene_tree_ptr);

/** Tears down tooltips. */
void wlmaker_tooltip_fini(void);

/**
 * Schedules the tooltip `text_ptr` for `element_ptr`. Replaces any other.
 *
 * @param owner_ptr           Identifies who asked, for @ref wlmaker_tooltip_hide.
 * @param text_ptr            Text to show. Copied.
 * @param element_ptr         The element to show it beside.
 */
void wlmaker_tooltip_show(
    void *owner_ptr,
    const char *text_ptr,
    wlmtk_element_t *element_ptr);

/** Hides (or cancels) the tooltip, if it was shown for `owner_ptr`. */
void wlmaker_tooltip_hide(void *owner_ptr);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __TOOLTIP_H__ */
/* == End of tooltip.h ===================================================== */

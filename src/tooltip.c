/* ========================================================================= */
/**
 * @file tooltip.c
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

#include "tooltip.h"

#include <cairo.h>
#include <libbase/libbase.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#undef WLR_USE_UNSTABLE

/* == Declarations ========================================================= */

/** State of the tooltip. There is only one: The pointer is at one place. */
typedef struct {
    /** Event loop, for the delay timer. */
    struct wl_event_loop      *wl_event_loop_ptr;
    /** Output layout, to keep the tooltip on the element's output. */
    struct wlr_output_layout  *wlr_output_layout_ptr;
    /** Where the tooltip goes. */
    struct wlr_scene_tree     *wlr_scene_tree_ptr;
    /** Timer: Shows the tooltip once the pointer rested long enough. */
    struct wl_event_source    *timer_ptr;
    /** The tooltip, when shown. */
    struct wlr_scene_buffer   *wlr_scene_buffer_ptr;
    /** Owner of the pending or shown tooltip. NULL if none. */
    void                      *owner_ptr;
    /** Text of the pending or shown tooltip. */
    char                      *text_ptr;
    /** Element the tooltip is for. */
    wlmtk_element_t           *element_ptr;
} wlmaker_tooltip_t;

static int _wlmaker_tooltip_handle_timer(void *data_ptr);
static struct wlr_buffer *_wlmaker_tooltip_render(
    const char *text_ptr, int *w_ptr, int *h_ptr);

/* == Data ================================================================= */

/** The tooltip. */
static wlmaker_tooltip_t _wlmaker_tooltip;
/** Delay until the tooltip shows, milliseconds. As Window Maker's balloons. */
static const int _wlmaker_tooltip_delay_ms = 600;
/** Gap between the element and the tooltip. */
static const int _wlmaker_tooltip_gap = 4;

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
bool wlmaker_tooltip_init(
    struct wl_event_loop *wl_event_loop_ptr,
    struct wlr_output_layout *wlr_output_layout_ptr,
    struct wlr_scene_tree *wlr_scene_tree_ptr)
{
    wlmaker_tooltip_t *t_ptr = &_wlmaker_tooltip;
    *t_ptr = (wlmaker_tooltip_t){
        .wl_event_loop_ptr = wl_event_loop_ptr,
        .wlr_output_layout_ptr = wlr_output_layout_ptr,
        .wlr_scene_tree_ptr = wlr_scene_tree_ptr,
    };
    t_ptr->timer_ptr = wl_event_loop_add_timer(
        wl_event_loop_ptr, _wlmaker_tooltip_handle_timer, t_ptr);
    return NULL != t_ptr->timer_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_tooltip_fini(void)
{
    wlmaker_tooltip_t *t_ptr = &_wlmaker_tooltip;
    wlmaker_tooltip_hide(t_ptr->owner_ptr);
    if (NULL != t_ptr->timer_ptr) {
        wl_event_source_remove(t_ptr->timer_ptr);
        t_ptr->timer_ptr = NULL;
    }
}

/* ------------------------------------------------------------------------- */
void wlmaker_tooltip_show(
    void *owner_ptr,
    const char *text_ptr,
    wlmtk_element_t *element_ptr)
{
    wlmaker_tooltip_t *t_ptr = &_wlmaker_tooltip;
    if (NULL == t_ptr->timer_ptr || NULL == owner_ptr) return;
    wlmaker_tooltip_hide(t_ptr->owner_ptr);
    if (NULL == text_ptr || 0 == strlen(text_ptr)) return;

    t_ptr->text_ptr = logged_strdup(text_ptr);
    if (NULL == t_ptr->text_ptr) return;
    t_ptr->owner_ptr = owner_ptr;
    t_ptr->element_ptr = element_ptr;
    wl_event_source_timer_update(t_ptr->timer_ptr, _wlmaker_tooltip_delay_ms);
}

/* ------------------------------------------------------------------------- */
void wlmaker_tooltip_hide(void *owner_ptr)
{
    wlmaker_tooltip_t *t_ptr = &_wlmaker_tooltip;
    if (NULL == owner_ptr || owner_ptr != t_ptr->owner_ptr) return;

    if (NULL != t_ptr->timer_ptr) {
        wl_event_source_timer_update(t_ptr->timer_ptr, 0);
    }
    if (NULL != t_ptr->wlr_scene_buffer_ptr) {
        wlr_scene_node_destroy(&t_ptr->wlr_scene_buffer_ptr->node);
        t_ptr->wlr_scene_buffer_ptr = NULL;
    }
    free(t_ptr->text_ptr);
    t_ptr->text_ptr = NULL;
    t_ptr->owner_ptr = NULL;
    t_ptr->element_ptr = NULL;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Timer fired: Shows the tooltip beside the element, on its output. */
int _wlmaker_tooltip_handle_timer(void *data_ptr)
{
    wlmaker_tooltip_t *t_ptr = data_ptr;
    if (NULL == t_ptr->owner_ptr || NULL == t_ptr->element_ptr ||
        NULL == t_ptr->element_ptr->wlr_scene_node_ptr ||
        !t_ptr->element_ptr->visible) return 0;

    int ex, ey;
    if (!wlr_scene_node_coords(t_ptr->element_ptr->wlr_scene_node_ptr,
                               &ex, &ey)) return 0;
    struct wlr_box e = wlmtk_element_get_dimensions_box(t_ptr->element_ptr);
    e.x += ex;
    e.y += ey;

    int w, h;
    struct wlr_buffer *wlr_buffer_ptr = _wlmaker_tooltip_render(
        t_ptr->text_ptr, &w, &h);
    if (NULL == wlr_buffer_ptr) return 0;

    // Beside the element, on the side with more room: Away from the edge
    // that the dock or clip sits on.
    struct wlr_box o = { .width = 0 };
    struct wlr_output *wlr_output_ptr = wlr_output_layout_output_at(
        t_ptr->wlr_output_layout_ptr,
        e.x + e.width / 2.0, e.y + e.height / 2.0);
    if (NULL != wlr_output_ptr) {
        wlr_output_layout_get_box(
            t_ptr->wlr_output_layout_ptr, wlr_output_ptr, &o);
    }
    int x, y;
    if (0 == o.width || e.y + e.height + _wlmaker_tooltip_gap + h <=
        o.y + o.height) {
        // Below the element, unless at the bottom of the output.
        x = e.x + (e.width - w) / 2;
        y = e.y + e.height + _wlmaker_tooltip_gap;
    } else {
        x = e.x + (e.width - w) / 2;
        y = e.y - _wlmaker_tooltip_gap - h;
    }
    if (0 < o.width) {
        x = BS_MAX(o.x, BS_MIN(x, o.x + o.width - w));
        y = BS_MAX(o.y, BS_MIN(y, o.y + o.height - h));
    }

    t_ptr->wlr_scene_buffer_ptr = wlr_scene_buffer_create(
        t_ptr->wlr_scene_tree_ptr, wlr_buffer_ptr);
    wlr_buffer_drop(wlr_buffer_ptr);
    if (NULL == t_ptr->wlr_scene_buffer_ptr) return 0;
    wlr_scene_node_set_position(&t_ptr->wlr_scene_buffer_ptr->node, x, y);
    wlr_scene_node_raise_to_top(&t_ptr->wlr_scene_buffer_ptr->node);
    return 0;
}

/* ------------------------------------------------------------------------- */
/** Renders the tooltip: Text in a light-yellow box, as Window Maker's. */
struct wlr_buffer *_wlmaker_tooltip_render(
    const char *text_ptr, int *w_ptr, int *h_ptr)
{
    cairo_surface_t *s_ptr = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *c_ptr = cairo_create(s_ptr);
    cairo_select_font_face(c_ptr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(c_ptr, 12);
    cairo_text_extents_t extents;
    cairo_text_extents(c_ptr, text_ptr, &extents);
    cairo_destroy(c_ptr);
    cairo_surface_destroy(s_ptr);

    *w_ptr = BS_MIN((int)extents.x_advance + 12, 600);
    *h_ptr = 20;
    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(
        *w_ptr, *h_ptr);
    if (NULL == wlr_buffer_ptr) return NULL;
    c_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == c_ptr) {
        wlr_buffer_drop(wlr_buffer_ptr);
        return NULL;
    }
    cairo_set_source_rgb(c_ptr, 1.0, 1.0, 0.88);
    cairo_paint(c_ptr);
    cairo_set_source_rgb(c_ptr, 0.0, 0.0, 0.0);
    cairo_set_line_width(c_ptr, 1.0);
    cairo_rectangle(c_ptr, 0.5, 0.5, *w_ptr - 1, *h_ptr - 1);
    cairo_stroke(c_ptr);
    cairo_select_font_face(c_ptr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(c_ptr, 12);
    cairo_move_to(c_ptr, 6,
                  *h_ptr / 2.0 - extents.y_bearing - extents.height / 2);
    cairo_show_text(c_ptr, text_ptr);
    cairo_destroy(c_ptr);
    return wlr_buffer_ptr;
}

/* == End of tooltip.c ===================================================== */

/* ========================================================================= */
/**
 * @file dock.c
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

#include "dock.h"

#include <inttypes.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "box.h"
#include "container.h"
#include "rectangle.h"

#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_scene.h>
#undef WLR_USE_UNSTABLE

/* == Declarations ========================================================= */

/** State of the toolkit dock. */
struct _wlmtk_dock_t {
    /** Parent class: The panel. */
    wlmtk_panel_t             super_panel;
    /** Original virtual method table of the panel's @ref wlmtk_element_t. */
    wlmtk_element_vmt_t       old_element_vmt;

    /** Positioning information for the panel. */
    wlmtk_panel_positioning_t panel_positioning;

    /** Copy of the positioning information this dock was called from. */
    wlmtk_dock_positioning_t  dock_positioning;
    /** Styling info of the dock. */
    struct wlmtk_dock_style   style;

    /**
     * Principal element of the dock is a box, holding the lines of tiles.
     * It is oriented across the dock's axis.
     */
    wlmtk_box_t               tile_box;
    /** Lines of tiles: Boxes along the dock's axis. Line 0 is at the edge. */
    wlmtk_box_t               line_boxes[WLMTK_DOCK_MAX_LINES];
    /** Number of lines to use, 1 .. @ref WLMTK_DOCK_MAX_LINES. */
    int                       lines;
    /** Snake order: Every other line runs the opposite way. */
    bool                      snake;
    /** Spacers, filling empty slots of a partial line. */
    wlmtk_rectangle_t         **spacers_ptr;
    /** Number of entries in `spacers_ptr`. */
    size_t                    spacers;
    /** Extra holder for tiles, in dock order, for direct access. */
    bs_dllist_t               tiles;
};

static uint32_t _wlmtk_dock_panel_request_size(
    wlmtk_panel_t *panel_ptr,
    int width,
    int height);
static void _wlmtk_dock_element_layout(wlmtk_element_t *element_ptr);

static wlmtk_box_orientation_t _wlmtk_dock_orientation(wlmtk_dock_t *dock_ptr);
static void _wlmtk_dock_clear_lines(wlmtk_dock_t *dock_ptr);
static void _wlmtk_dock_arrange(wlmtk_dock_t *dock_ptr);
static bool _wlmtk_dock_positioning(
    wlmtk_dock_t *dock_ptr,
    wlmtk_panel_positioning_t *panel_positioning_ptr);
static bool _wlmtk_dock_tile_set_style(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);
static void _wlmtk_dock_tile_destroy(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);

/* == Data ================================================================= */

const bspl_desc_t wlmtk_dock_style_desc[] = {
    BSPL_DESC_DICT(
        "Margin", true, struct wlmtk_dock_style, margin, margin,
        wlmtk_style_margin_desc),
    BSPL_DESC_SENTINEL()
};

/** Virtual method table of the panel. */
static const wlmtk_panel_vmt_t _wlmtk_dock_panel_vmt = {
    .request_size = _wlmtk_dock_panel_request_size
};

/** Virtual method table of the panel's element. */
static const wlmtk_element_vmt_t _wlmtk_dock_element_vmt = {
    .layout = _wlmtk_dock_element_layout
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmtk_dock_t *wlmtk_dock_create(
    const wlmtk_dock_positioning_t *dock_positioning_ptr,
    const struct wlmtk_dock_style *style_ptr)
{
    wlmtk_dock_t *dock_ptr = logged_calloc(1, sizeof(*dock_ptr));
    if (NULL == dock_ptr) return NULL;
    dock_ptr->dock_positioning = *dock_positioning_ptr;
    dock_ptr->style = *style_ptr;

    dock_ptr->lines = 1;
    wlmtk_box_orientation_t axis = _wlmtk_dock_orientation(dock_ptr);
    if (!wlmtk_box_init(
            &dock_ptr->tile_box,
            WLMTK_BOX_VERTICAL == axis ?
            WLMTK_BOX_HORIZONTAL : WLMTK_BOX_VERTICAL,
            &dock_ptr->style.margin)) {
        wlmtk_dock_destroy(dock_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(wlmtk_box_element(&dock_ptr->tile_box), true);
    for (int l = 0; l < WLMTK_DOCK_MAX_LINES; ++l) {
        if (!wlmtk_box_init(&dock_ptr->line_boxes[l], axis,
                            &dock_ptr->style.margin)) {
            wlmtk_dock_destroy(dock_ptr);
            return NULL;
        }
        wlmtk_element_set_visible(
            wlmtk_box_element(&dock_ptr->line_boxes[l]), true);
    }
    _wlmtk_dock_arrange(dock_ptr);

    if (!_wlmtk_dock_positioning(
            dock_ptr,
            &dock_ptr->panel_positioning)) {
        wlmtk_dock_destroy(dock_ptr);
        return NULL;
    }

    if (!wlmtk_panel_init(
            &dock_ptr->super_panel,
            &dock_ptr->panel_positioning)) {
        bs_log(BS_ERROR, "Failed wlmtk_panel_init.");
        wlmtk_dock_destroy(dock_ptr);
        return NULL;
    }
    wlmtk_panel_extend(&dock_ptr->super_panel, &_wlmtk_dock_panel_vmt);
    dock_ptr->old_element_vmt = wlmtk_element_extend(
        wlmtk_dock_element(dock_ptr), &_wlmtk_dock_element_vmt);

    wlmtk_container_add_element(
        &dock_ptr->super_panel.super_container,
        wlmtk_box_element(&dock_ptr->tile_box));

    return dock_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmtk_dock_destroy(wlmtk_dock_t *dock_ptr)
{
    bs_dllist_for_each(&dock_ptr->tiles, _wlmtk_dock_tile_destroy, dock_ptr);
    _wlmtk_dock_clear_lines(dock_ptr);
    for (int l = 0; l < WLMTK_DOCK_MAX_LINES; ++l) {
        wlmtk_box_fini(&dock_ptr->line_boxes[l]);
    }
    free(dock_ptr->spacers_ptr);

    if (wlmtk_box_element(&dock_ptr->tile_box)->parent_container_ptr) {
        wlmtk_container_remove_element(
            &dock_ptr->super_panel.super_container,
            wlmtk_box_element(&dock_ptr->tile_box));
        wlmtk_box_fini(&dock_ptr->tile_box);
    }

    wlmtk_panel_fini(&dock_ptr->super_panel);
    free(dock_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmtk_dock_set_style(
    wlmtk_dock_t *dock_ptr,
    const struct wlmtk_dock_style *style_ptr,
    const struct wlmtk_tile_style *tile_style_ptr)
{
    wlmtk_box_set_style(&dock_ptr->tile_box, &style_ptr->margin);
    for (int l = 0; l < WLMTK_DOCK_MAX_LINES; ++l) {
        wlmtk_box_set_style(&dock_ptr->line_boxes[l], &style_ptr->margin);
    }
    bool rv = bs_dllist_all(
        &dock_ptr->tiles,
        _wlmtk_dock_tile_set_style,
        (void*)tile_style_ptr);

    _wlmtk_dock_arrange(dock_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
void wlmtk_dock_add_tile(
    wlmtk_dock_t *dock_ptr,
    wlmtk_tile_t *tile_ptr)
{
    BS_ASSERT(NULL == wlmtk_tile_element(tile_ptr)->parent_container_ptr);
    bs_dllist_push_back(&dock_ptr->tiles, wlmtk_dlnode_from_tile(tile_ptr));
    _wlmtk_dock_arrange(dock_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmtk_dock_remove_tile(
    wlmtk_dock_t *dock_ptr,
    wlmtk_tile_t *tile_ptr)
{
    BS_ASSERT(bs_dllist_contains(&dock_ptr->tiles,
                                 wlmtk_dlnode_from_tile(tile_ptr)));
    wlmtk_element_t *element_ptr = wlmtk_tile_element(tile_ptr);
    if (NULL != element_ptr->parent_container_ptr) {
        wlmtk_container_remove_element(
            element_ptr->parent_container_ptr, element_ptr);
    }
    bs_dllist_remove(&dock_ptr->tiles, wlmtk_dlnode_from_tile(tile_ptr));
    _wlmtk_dock_arrange(dock_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmtk_dock_set_positioning(
    wlmtk_dock_t *dock_ptr,
    const wlmtk_dock_positioning_t *dock_positioning_ptr)
{
    wlmtk_dock_positioning_t old_positioning = dock_ptr->dock_positioning;
    dock_ptr->dock_positioning = *dock_positioning_ptr;
    wlmtk_panel_positioning_t panel_positioning = {};
    if (!_wlmtk_dock_positioning(dock_ptr, &panel_positioning)) {
        dock_ptr->dock_positioning = old_positioning;
        return false;
    }

    _wlmtk_dock_clear_lines(dock_ptr);
    wlmtk_box_orientation_t axis = _wlmtk_dock_orientation(dock_ptr);
    dock_ptr->tile_box.orientation = WLMTK_BOX_VERTICAL == axis ?
        WLMTK_BOX_HORIZONTAL : WLMTK_BOX_VERTICAL;
    // Boxes position elements along their axis only: Clear the other
    // coordinate, left over from the previous orientation.
    for (int l = 0; l < WLMTK_DOCK_MAX_LINES; ++l) {
        dock_ptr->line_boxes[l].orientation = axis;
        wlmtk_element_set_position(
            wlmtk_box_element(&dock_ptr->line_boxes[l]), 0, 0);
    }
    for (bs_dllist_node_t *dlnode_ptr = dock_ptr->tiles.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmtk_element_set_position(
            wlmtk_tile_element(wlmtk_tile_from_dlnode(dlnode_ptr)), 0, 0);
    }
    _wlmtk_dock_arrange(dock_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
void wlmtk_dock_set_lines(wlmtk_dock_t *dock_ptr, int lines, bool snake)
{
    dock_ptr->lines = BS_MIN(BS_MAX(lines, 1), WLMTK_DOCK_MAX_LINES);
    dock_ptr->snake = snake;
    _wlmtk_dock_arrange(dock_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmtk_dock_move_tile(
    wlmtk_dock_t *dock_ptr,
    wlmtk_tile_t *tile_ptr,
    size_t index)
{
    bs_dllist_node_t *dlnode_ptr = wlmtk_dlnode_from_tile(tile_ptr);
    BS_ASSERT(bs_dllist_contains(&dock_ptr->tiles, dlnode_ptr));
    bs_dllist_remove(&dock_ptr->tiles, dlnode_ptr);

    bs_dllist_node_t *at_ptr = dock_ptr->tiles.head_ptr;
    for (size_t i = 0; i < index && NULL != at_ptr; ++i) at_ptr = at_ptr->next_ptr;
    if (NULL == at_ptr) {
        bs_dllist_push_back(&dock_ptr->tiles, dlnode_ptr);
    } else {
        bs_dllist_insert_node_before(&dock_ptr->tiles, at_ptr, dlnode_ptr);
    }
    _wlmtk_dock_arrange(dock_ptr);
}

/* ------------------------------------------------------------------------- */
size_t wlmtk_dock_tile_index(wlmtk_dock_t *dock_ptr, wlmtk_tile_t *tile_ptr)
{
    size_t i = 0;
    for (bs_dllist_node_t *dlnode_ptr = dock_ptr->tiles.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr, ++i) {
        if (wlmtk_tile_from_dlnode(dlnode_ptr) == tile_ptr) return i;
    }
    return SIZE_MAX;
}

/* ------------------------------------------------------------------------- */
wlmtk_tile_t *wlmtk_dock_tile_at(wlmtk_dock_t *dock_ptr, double x, double y)
{
    for (bs_dllist_node_t *dlnode_ptr = dock_ptr->tiles.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmtk_tile_t *tile_ptr = wlmtk_tile_from_dlnode(dlnode_ptr);
        wlmtk_element_t *element_ptr = wlmtk_tile_element(tile_ptr);
        if (NULL == element_ptr->wlr_scene_node_ptr) continue;
        int nx, ny;
        if (!wlr_scene_node_coords(element_ptr->wlr_scene_node_ptr, &nx, &ny)) {
            continue;
        }
        int size = tile_ptr->style.size;
        if (x >= nx && x < nx + size && y >= ny && y < ny + size) {
            return tile_ptr;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
wlmtk_panel_t *wlmtk_dock_panel(wlmtk_dock_t *dock_ptr)
{
    return &dock_ptr->super_panel;
}

/* ------------------------------------------------------------------------- */
wlmtk_element_t *wlmtk_dock_element(wlmtk_dock_t *dock_ptr)
{
    return wlmtk_panel_element(wlmtk_dock_panel(dock_ptr));
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Recomputes the dock's size -- and also commits it.
 *
 * @param panel_ptr
 * @param width
 * @param height
 *
 * @return 0
 */
uint32_t _wlmtk_dock_panel_request_size(
    wlmtk_panel_t *panel_ptr,
    __UNUSED__ int width,
    __UNUSED__ int height)
{
    wlmtk_dock_t *dock_ptr = BS_CONTAINER_OF(
        panel_ptr, wlmtk_dock_t, super_panel);

    wlmtk_panel_positioning_t panel_positioning = {};
    if (!_wlmtk_dock_positioning(dock_ptr, &panel_positioning)) {
        bs_log(BS_ERROR, "Panel %p invalid positioning data.", panel_ptr);
        return 0;
    }

    wlmtk_panel_commit(panel_ptr, 0, &panel_positioning);
    return 0;
}

/* ------------------------------------------------------------------------- */
/** Redraw the dock. We also (re)request size, ie. the positioning. */
void _wlmtk_dock_element_layout(wlmtk_element_t *element_ptr)
{
    wlmtk_dock_t *dock_ptr = BS_CONTAINER_OF(
        element_ptr, wlmtk_dock_t, super_panel.super_container.super_element);
    dock_ptr->old_element_vmt.layout(element_ptr);
    _wlmtk_dock_panel_request_size(&dock_ptr->super_panel, 0, 0);
}

/* ------------------------------------------------------------------------- */
/** Removes all tiles and spacers from the lines, and lines from the dock. */
void _wlmtk_dock_clear_lines(wlmtk_dock_t *dock_ptr)
{
    for (bs_dllist_node_t *dlnode_ptr = dock_ptr->tiles.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmtk_element_t *element_ptr = wlmtk_tile_element(
            wlmtk_tile_from_dlnode(dlnode_ptr));
        if (NULL != element_ptr->parent_container_ptr) {
            wlmtk_container_remove_element(
                element_ptr->parent_container_ptr, element_ptr);
        }
    }
    for (size_t i = 0; i < dock_ptr->spacers; ++i) {
        wlmtk_element_t *element_ptr = wlmtk_rectangle_element(
            dock_ptr->spacers_ptr[i]);
        if (NULL != element_ptr->parent_container_ptr) {
            wlmtk_container_remove_element(
                element_ptr->parent_container_ptr, element_ptr);
        }
        wlmtk_rectangle_destroy(dock_ptr->spacers_ptr[i]);
    }
    dock_ptr->spacers = 0;
    for (int l = 0; l < WLMTK_DOCK_MAX_LINES; ++l) {
        wlmtk_element_t *element_ptr = wlmtk_box_element(
            &dock_ptr->line_boxes[l]);
        if (NULL != element_ptr->parent_container_ptr) {
            wlmtk_box_remove_element(&dock_ptr->tile_box, element_ptr);
        }
    }
}

/* ------------------------------------------------------------------------- */
/**
 * (Re)arranges the tiles into lines: Line 0 at the screen edge, each line
 * filled from the anchor. With `snake`, every other line runs back toward
 * the anchor, so the order continues where the previous line ended; empty
 * slots of a partial line get spacers.
 */
void _wlmtk_dock_arrange(wlmtk_dock_t *dock_ptr)
{
    _wlmtk_dock_clear_lines(dock_ptr);

    size_t n = bs_dllist_size(&dock_ptr->tiles);
    size_t lines_wanted = (size_t)dock_ptr->lines;
    int lines = (int)(n < lines_wanted ? (n < 1 ? 1 : n) : lines_wanted);
    size_t per_line = (n + (size_t)lines - 1) / (size_t)lines;
    if (per_line < 1) per_line = 1;
    bool edge_at_end = WLR_EDGE_RIGHT == dock_ptr->dock_positioning.edge ||
        WLR_EDGE_BOTTOM == dock_ptr->dock_positioning.edge;
    bool anchor_at_start = WLR_EDGE_TOP == dock_ptr->dock_positioning.anchor ||
        WLR_EDGE_LEFT == dock_ptr->dock_positioning.anchor;
    int size = 0;
    if (NULL != dock_ptr->tiles.head_ptr) {
        size = wlmtk_tile_from_dlnode(dock_ptr->tiles.head_ptr)->style.size;
    }

    if (1 < lines) {
        wlmtk_rectangle_t **spacers_ptr = realloc(
            dock_ptr->spacers_ptr,
            sizeof(wlmtk_rectangle_t*) * lines * per_line);
        if (NULL == spacers_ptr) lines = 1;
        else dock_ptr->spacers_ptr = spacers_ptr;
    }
    if (1 == lines) per_line = n;

    bs_dllist_node_t *dlnode_ptr = dock_ptr->tiles.head_ptr;
    for (int l = 0; l < lines; ++l) {
        wlmtk_box_t *line_ptr = &dock_ptr->line_boxes[l];
        if (edge_at_end) {
            wlmtk_box_add_element_front(
                &dock_ptr->tile_box, wlmtk_box_element(line_ptr));
        } else {
            wlmtk_box_add_element_back(
                &dock_ptr->tile_box, wlmtk_box_element(line_ptr));
        }

        // Slot k is k-th from the anchor.
        wlmtk_element_t *slots[per_line > 0 ? per_line : 1];
        for (size_t k = 0; k < per_line; ++k) slots[k] = NULL;
        for (size_t i = 0; i < per_line && NULL != dlnode_ptr;
             ++i, dlnode_ptr = dlnode_ptr->next_ptr) {
            size_t k = (dock_ptr->snake && (l & 1)) ? per_line - 1 - i : i;
            slots[k] = wlmtk_tile_element(wlmtk_tile_from_dlnode(dlnode_ptr));
        }

        for (size_t k = 0; k < per_line; ++k) {
            wlmtk_element_t *element_ptr = slots[k];
            if (NULL == element_ptr) {
                wlmtk_rectangle_t *spacer_ptr = wlmtk_rectangle_create(
                    size, size, 0x00000000);
                if (NULL == spacer_ptr) continue;
                dock_ptr->spacers_ptr[dock_ptr->spacers++] = spacer_ptr;
                element_ptr = wlmtk_rectangle_element(spacer_ptr);
                wlmtk_element_set_visible(element_ptr, true);
            }
            if (anchor_at_start) {
                wlmtk_box_add_element_back(line_ptr, element_ptr);
            } else {
                wlmtk_box_add_element_front(line_ptr, element_ptr);
            }
        }
    }

    wlmtk_element_layout(wlmtk_box_element(&dock_ptr->tile_box));
    _wlmtk_dock_panel_request_size(&dock_ptr->super_panel, 0, 0);
}

/* ------------------------------------------------------------------------- */
/** Derives the box' orientation for the dock. */
wlmtk_box_orientation_t _wlmtk_dock_orientation(wlmtk_dock_t *dock_ptr)
{
    switch (dock_ptr->dock_positioning.edge) {
    case WLR_EDGE_LEFT:
    case WLR_EDGE_RIGHT:
        return WLMTK_BOX_VERTICAL;

    case WLR_EDGE_TOP:
    case WLR_EDGE_BOTTOM:
        return WLMTK_BOX_HORIZONTAL;

    default:
        bs_log(BS_FATAL, "wlmtk_dock %p: Invalid edge %"PRIx32,
               dock_ptr, dock_ptr->dock_positioning.edge);
    }
    /* Not reached. */
    return WLMTK_BOX_HORIZONTAL;
}

/* ------------------------------------------------------------------------- */
/** Fills the panel positioning parameters from the dock's. */
bool _wlmtk_dock_positioning(
    wlmtk_dock_t *dock_ptr,
    wlmtk_panel_positioning_t *panel_positioning_ptr)
{
    struct wlr_box box = wlmtk_element_get_dimensions_box(
        wlmtk_box_element(&dock_ptr->tile_box));

    switch (dock_ptr->dock_positioning.edge) {
    case WLR_EDGE_LEFT:
    case WLR_EDGE_RIGHT:
        panel_positioning_ptr->anchor = dock_ptr->dock_positioning.edge;
        panel_positioning_ptr->desired_width = BS_MAX(box.width, 1);

        if (dock_ptr->dock_positioning.anchor != WLR_EDGE_TOP &&
            dock_ptr->dock_positioning.anchor != WLR_EDGE_BOTTOM) {
            bs_log(BS_ERROR, "wlmtk_dock_t anchor must be adjacent to edge: "
                   "anchor %"PRIx32", edge %"PRIx32,
                   dock_ptr->dock_positioning.anchor,
                   dock_ptr->dock_positioning.edge);
            return false;
        }
        panel_positioning_ptr->anchor |= dock_ptr->dock_positioning.anchor;

        // The layer protocol requires a non-zero value for panels not spanning
        // the entire height. Go with a one-tile dimension, as long as there's
        // no tiles yet.
        panel_positioning_ptr->desired_height = BS_MAX(box.height, 1);
        break;

    case WLR_EDGE_TOP:
    case WLR_EDGE_BOTTOM:
        panel_positioning_ptr->anchor = dock_ptr->dock_positioning.edge;
        panel_positioning_ptr->desired_height = BS_MAX(box.height, 1);
        if (dock_ptr->dock_positioning.anchor != WLR_EDGE_LEFT &&
            dock_ptr->dock_positioning.anchor != WLR_EDGE_RIGHT) {
            bs_log(BS_ERROR, "wlmtk_dock_t anchor must be adjacent to edge: "
                   "anchor %"PRIx32", edge %"PRIx32,
                   dock_ptr->dock_positioning.anchor,
                   dock_ptr->dock_positioning.edge);
            return false;
        }
        panel_positioning_ptr->anchor |= dock_ptr->dock_positioning.anchor;

        // The layer protocol requires a non-zero value for panels not spanning
        // the entire width. Go with a one-tile dimension, as long as there's
        // no tiles yet.
        panel_positioning_ptr->desired_width = BS_MAX(box.width, 1);
        break;

    default:
        bs_log(BS_ERROR, "Unexpected wlmtk_dock_t positioning edge 0x%"PRIx32,
               dock_ptr->dock_positioning.edge);
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------------- */
/** Iterator to bs_dllist_all(): Sets style for a tile in the iterator. */
bool _wlmtk_dock_tile_set_style(bs_dllist_node_t *dlnode_ptr, void *ud_ptr)
{
    wlmtk_tile_t *tile_ptr = wlmtk_tile_from_dlnode(dlnode_ptr);
    const struct wlmtk_tile_style *tile_style_ptr = ud_ptr;
    return wlmtk_tile_set_style(tile_ptr, tile_style_ptr);
}

/* ------------------------------------------------------------------------- */
/** Iterator to bs_dllist_for_each(): Removes & destroys the tile at node. */
void _wlmtk_dock_tile_destroy(bs_dllist_node_t *dlnode_ptr, void *ud_ptr)
{
    wlmtk_tile_t *tile_ptr = wlmtk_tile_from_dlnode(dlnode_ptr);
    wlmtk_dock_t *dock_ptr = ud_ptr;
    wlmtk_dock_remove_tile(dock_ptr, tile_ptr);
    wlmtk_element_destroy(wlmtk_tile_element(tile_ptr));
}

/* == Unit tests =========================================================== */

static void test_create_destroy(bs_test_t *test_ptr);
static void test_lines_and_move(bs_test_t *test_ptr);

/** Test cases */
static const bs_test_case_t _wlmtk_dock_test_cases[] = {
    { 1, "create_destroy", test_create_destroy },
    { 1, "lines_and_move", test_lines_and_move },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmtk_dock_test_set = BS_TEST_SET(
    true, "dock", _wlmtk_dock_test_cases);

/* ------------------------------------------------------------------------- */
/** Exercises ctor and dtor. */
 void test_create_destroy(bs_test_t *test_ptr)
{
    wlmtk_dock_positioning_t pos = {
        .edge = WLR_EDGE_LEFT,
        .anchor = WLR_EDGE_BOTTOM,
    };
    struct wlmtk_dock_style style = {};

    wlmtk_dock_t *dock_ptr = wlmtk_dock_create(&pos, &style);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLR_EDGE_LEFT | WLR_EDGE_BOTTOM,
        dock_ptr->super_panel.positioning.anchor);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        1,
        dock_ptr->super_panel.positioning.desired_width);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        1,
        dock_ptr->super_panel.positioning.desired_height);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLMTK_BOX_HORIZONTAL,
        dock_ptr->tile_box.orientation);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLMTK_BOX_VERTICAL,
        dock_ptr->line_boxes[0].orientation);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, dock_ptr);
    wlmtk_dock_destroy(dock_ptr);
}

/* ------------------------------------------------------------------------- */
/** Arranges tiles in lines, with snake order, and moves tiles. */
void test_lines_and_move(bs_test_t *test_ptr)
{
    wlmtk_dock_positioning_t pos = {
        .edge = WLR_EDGE_RIGHT,
        .anchor = WLR_EDGE_TOP,
    };
    struct wlmtk_dock_style style = {};
    struct wlmtk_tile_style tile_style = { .size = 64, .content_size = 56 };
    wlmtk_dock_t *dock_ptr = wlmtk_dock_create(&pos, &style);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dock_ptr);

    wlmtk_tile_t tiles[5];
    for (int i = 0; i < 5; ++i) {
        BS_TEST_VERIFY_TRUE_OR_RETURN(
            test_ptr, wlmtk_tile_init(&tiles[i], &tile_style));
        wlmtk_element_set_visible(wlmtk_tile_element(&tiles[i]), true);
        wlmtk_dock_add_tile(dock_ptr, &tiles[i]);
    }

    // One line: All tiles in line 0, 5 high.
    struct wlr_box box = wlmtk_element_get_dimensions_box(
        wlmtk_box_element(&dock_ptr->tile_box));
    BS_TEST_VERIFY_EQ(test_ptr, 64, box.width);
    BS_TEST_VERIFY_EQ(test_ptr, 5 * 64, box.height);

    // Two lines, snake: 3 per line; line 1 holds tiles 3, 4 and a spacer.
    wlmtk_dock_set_lines(dock_ptr, 2, true);
    box = wlmtk_element_get_dimensions_box(
        wlmtk_box_element(&dock_ptr->tile_box));
    BS_TEST_VERIFY_EQ(test_ptr, 2 * 64, box.width);
    BS_TEST_VERIFY_EQ(test_ptr, 3 * 64, box.height);
    BS_TEST_VERIFY_EQ(test_ptr, 1, dock_ptr->spacers);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        &dock_ptr->line_boxes[1].element_container,
        wlmtk_tile_element(&tiles[4])->parent_container_ptr);

    // Move tile 0 to the end; tile 1 becomes first.
    wlmtk_dock_move_tile(dock_ptr, &tiles[0], 4);
    BS_TEST_VERIFY_EQ(test_ptr, 4, wlmtk_dock_tile_index(dock_ptr, &tiles[0]));
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmtk_dock_tile_index(dock_ptr, &tiles[1]));
    wlmtk_dock_move_tile(dock_ptr, &tiles[0], 0);
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmtk_dock_tile_index(dock_ptr, &tiles[0]));

    for (int i = 0; i < 5; ++i) {
        wlmtk_dock_remove_tile(dock_ptr, &tiles[i]);
        wlmtk_tile_fini(&tiles[i]);
    }
    BS_TEST_VERIFY_EQ(test_ptr, 0, dock_ptr->spacers);
    wlmtk_dock_destroy(dock_ptr);
}

/* == End of dock.c ======================================================== */

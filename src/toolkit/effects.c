/* ========================================================================= */
/**
 * @file effects.c
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

#include "effects.h"
#include "test.h"

#include <libbase/libbase.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>

#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_scene.h>
#undef WLR_USE_UNSTABLE

/* == Declarations ========================================================= */

/** A running animation. */
typedef struct {
    /** Element of `_wlmtk_effects_animations`. */
    bs_dllist_node_t          dlnode;
    /** The animated element. */
    wlmtk_element_t           *element_ptr;
    /**
     * Vertical offset currently applied to the element. Animations only add
     * their offset on top of the element's position, so moving the element
     * while animating (eg. a menu placed at the pointer) still works.
     */
    int                       applied_dy;
    /** The y position this animation last set; differs if moved since. */
    int                       last_y;
    /** Whether `last_y` is valid. */
    bool                      has_last_y;
    /** Opacity range. */
    double                    from_opacity, to_opacity;
    /** Vertical offset range, relative to the base position. */
    int                       from_dy, to_dy;
    /** Start time, in milliseconds (monotonic). */
    uint64_t                  start_msec;
    /** Called when done. */
    wlmtk_effects_done_fn     done_fn;
    /** Argument to `done_fn`. */
    void                      *ud_ptr;
} _wlmtk_animation_t;

/** Animation frame interval, ~60 Hz. */
static const int _wlmtk_effects_frame_msec = 16;

/** Current effects: Opaque, no animations. */
static wlmtk_effects_t _wlmtk_effects = {
    .active_opacity = 1.0,
    .inactive_opacity = 1.0,
    .animate_minimize = false,
    .animate_map = false,
    .duration_ms = 0,
};
/** Event loop, for the animation timer. */
static struct wl_event_loop *_wlmtk_effects_loop_ptr;
/** Animation timer. Created on first use. */
static struct wl_event_source *_wlmtk_effects_timer_ptr;
/** Running animations. */
static bs_dllist_t _wlmtk_effects_animations;

static int _wlmtk_effects_handle_timer(void *data_ptr);
static _wlmtk_animation_t *_wlmtk_effects_find(wlmtk_element_t *element_ptr);
static void _wlmtk_effects_set_dy(_wlmtk_animation_t *a_ptr, int dy);
static void _wlmtk_effects_set_opacity_iterator(
    struct wlr_scene_buffer *buffer_ptr, int sx, int sy, void *ud_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
void wlmtk_effects_set(
    const wlmtk_effects_t *effects_ptr,
    struct wl_event_loop *wl_event_loop_ptr)
{
    _wlmtk_effects = *effects_ptr;
    _wlmtk_effects.active_opacity = BS_MIN(
        BS_MAX(_wlmtk_effects.active_opacity, 0.1), 1.0);
    _wlmtk_effects.inactive_opacity = BS_MIN(
        BS_MAX(_wlmtk_effects.inactive_opacity, 0.1), 1.0);
    _wlmtk_effects.duration_ms = BS_MIN(
        BS_MAX(_wlmtk_effects.duration_ms, 0), 2000);
    _wlmtk_effects_loop_ptr = wl_event_loop_ptr;
}

/* ------------------------------------------------------------------------- */
const wlmtk_effects_t *wlmtk_effects_get(void)
{
    return &_wlmtk_effects;
}

/* ------------------------------------------------------------------------- */
bool wlmtk_effects_can_animate(void)
{
    return NULL != _wlmtk_effects_loop_ptr && 0 < _wlmtk_effects.duration_ms;
}

/* ------------------------------------------------------------------------- */
void wlmtk_effects_apply_opacity(wlmtk_element_t *element_ptr, double opacity)
{
    if (NULL == element_ptr->wlr_scene_node_ptr) return;
    float o = opacity;
    wlr_scene_node_for_each_buffer(
        element_ptr->wlr_scene_node_ptr,
        _wlmtk_effects_set_opacity_iterator,
        &o);
}

/* ------------------------------------------------------------------------- */
bool wlmtk_effects_animate(
    wlmtk_element_t *element_ptr,
    double from_opacity,
    double to_opacity,
    int from_dy,
    int to_dy,
    wlmtk_effects_done_fn done_fn,
    void *ud_ptr)
{
    if (!wlmtk_effects_can_animate()) return false;
    if (NULL == _wlmtk_effects_timer_ptr) {
        _wlmtk_effects_timer_ptr = wl_event_loop_add_timer(
            _wlmtk_effects_loop_ptr, _wlmtk_effects_handle_timer, NULL);
        if (NULL == _wlmtk_effects_timer_ptr) return false;
    }

    // A running animation of this element: Start from its base position.
    wlmtk_effects_cancel(element_ptr);

    _wlmtk_animation_t *a_ptr = logged_calloc(1, sizeof(_wlmtk_animation_t));
    if (NULL == a_ptr) return false;
    a_ptr->element_ptr = element_ptr;
    a_ptr->from_opacity = from_opacity;
    a_ptr->to_opacity = to_opacity;
    a_ptr->from_dy = from_dy;
    a_ptr->to_dy = to_dy;
    a_ptr->start_msec = bs_mono_nsec() / 1000000;
    a_ptr->done_fn = done_fn;
    a_ptr->ud_ptr = ud_ptr;
    bs_dllist_push_back(&_wlmtk_effects_animations, &a_ptr->dlnode);

    wlmtk_effects_apply_opacity(element_ptr, from_opacity);
    _wlmtk_effects_set_dy(a_ptr, from_dy);
    wl_event_source_timer_update(
        _wlmtk_effects_timer_ptr, _wlmtk_effects_frame_msec);
    return true;
}

/* ------------------------------------------------------------------------- */
bool wlmtk_effects_cancel(wlmtk_element_t *element_ptr)
{
    _wlmtk_animation_t *a_ptr = _wlmtk_effects_find(element_ptr);
    if (NULL == a_ptr) return false;
    _wlmtk_effects_set_dy(a_ptr, 0);
    bs_dllist_remove(&_wlmtk_effects_animations, &a_ptr->dlnode);
    free(a_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
bool wlmtk_effects_is_animating(wlmtk_element_t *element_ptr)
{
    return NULL != _wlmtk_effects_find(element_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** @return The animation of `element_ptr`, or NULL. */
_wlmtk_animation_t *_wlmtk_effects_find(wlmtk_element_t *element_ptr)
{
    for (bs_dllist_node_t *dlnode_ptr = _wlmtk_effects_animations.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        _wlmtk_animation_t *a_ptr = BS_CONTAINER_OF(
            dlnode_ptr, _wlmtk_animation_t, dlnode);
        if (a_ptr->element_ptr == element_ptr) return a_ptr;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/** Replaces the applied vertical offset by `dy`, keeping other moves. */
void _wlmtk_effects_set_dy(_wlmtk_animation_t *a_ptr, int dy)
{
    int x, y;
    wlmtk_element_get_position(a_ptr->element_ptr, &x, &y);
    // Moved by someone else since? Then that position is the base now.
    int base_y = (a_ptr->has_last_y && y == a_ptr->last_y) ?
        y - a_ptr->applied_dy : y;
    if (base_y + dy != y) {
        wlmtk_element_set_position(a_ptr->element_ptr, x, base_y + dy);
    }
    a_ptr->applied_dy = dy;
    a_ptr->last_y = base_y + dy;
    a_ptr->has_last_y = true;
}

/* ------------------------------------------------------------------------- */
/** Advances all animations; completes those that are due. */
int _wlmtk_effects_handle_timer(__UNUSED__ void *data_ptr)
{
    uint64_t now_msec = bs_mono_nsec() / 1000000;
    int duration = BS_MAX(_wlmtk_effects.duration_ms, 1);

    bs_dllist_node_t *dlnode_ptr = _wlmtk_effects_animations.head_ptr;
    while (NULL != dlnode_ptr) {
        _wlmtk_animation_t *a_ptr = BS_CONTAINER_OF(
            dlnode_ptr, _wlmtk_animation_t, dlnode);
        dlnode_ptr = dlnode_ptr->next_ptr;

        double t = (double)(now_msec - a_ptr->start_msec) / duration;
        if (t > 1.0) t = 1.0;
        // Ease out: Fast start, gentle end.
        double e = 1.0 - (1.0 - t) * (1.0 - t);
        wlmtk_effects_apply_opacity(
            a_ptr->element_ptr,
            a_ptr->from_opacity + (a_ptr->to_opacity - a_ptr->from_opacity) * e);
        _wlmtk_effects_set_dy(
            a_ptr,
            a_ptr->from_dy + (int)((a_ptr->to_dy - a_ptr->from_dy) * e));
        if (t < 1.0) continue;

        // Back at the element's own position: The offset was just visual.
        _wlmtk_effects_set_dy(a_ptr, 0);
        bs_dllist_remove(&_wlmtk_effects_animations, &a_ptr->dlnode);
        wlmtk_effects_done_fn done_fn = a_ptr->done_fn;
        wlmtk_element_t *element_ptr = a_ptr->element_ptr;
        void *ud_ptr = a_ptr->ud_ptr;
        free(a_ptr);
        // May start or cancel animations: Continue from the list's head.
        if (NULL != done_fn) done_fn(element_ptr, ud_ptr);
        dlnode_ptr = _wlmtk_effects_animations.head_ptr;
    }

    if (NULL != _wlmtk_effects_animations.head_ptr) {
        wl_event_source_timer_update(
            _wlmtk_effects_timer_ptr, _wlmtk_effects_frame_msec);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/** Iterator for wlr_scene_node_for_each_buffer: Sets the opacity. */
void _wlmtk_effects_set_opacity_iterator(
    struct wlr_scene_buffer *buffer_ptr,
    __UNUSED__ int sx,
    __UNUSED__ int sy,
    void *ud_ptr)
{
    wlr_scene_buffer_set_opacity(buffer_ptr, *(float*)ud_ptr);
}

/* == Unit tests =========================================================== */

static void test_move_while_animating(bs_test_t *test_ptr);

/** Test cases. */
static const bs_test_case_t _wlmtk_effects_test_cases[] = {
    { 1, "move_while_animating", test_move_while_animating },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmtk_effects_test_set = BS_TEST_SET(
    true, "effects", _wlmtk_effects_test_cases);

/** Done callback: Counts calls. */
static void _test_done(__UNUSED__ wlmtk_element_t *e, void *ud_ptr)
{
    ++*(int*)ud_ptr;
}

/* ------------------------------------------------------------------------- */
/**
 * An element moved while animating (a menu placed at the pointer just after
 * mapping) ends up where it was moved to, not at its position at start.
 */
void test_move_while_animating(bs_test_t *test_ptr)
{
    struct wl_event_loop *loop_ptr = wl_event_loop_create();
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, loop_ptr);
    wlmtk_effects_t effects = {
        .active_opacity = 1.0, .inactive_opacity = 1.0,
        .animate_map = true, .duration_ms = 40 };
    wlmtk_effects_set(&effects, loop_ptr);

    wlmtk_fake_element_t *fe_ptr = wlmtk_fake_element_create();
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, fe_ptr);
    int done = 0;
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_effects_animate(
                            &fe_ptr->element, 0.0, 1.0, 20, 0,
                            _test_done, &done));
    // Moved while the 20px offset is applied.
    wlmtk_element_set_position(&fe_ptr->element, 300, 200);

    for (int i = 0; i < 50 && 0 == done; ++i) {
        wl_event_loop_dispatch(loop_ptr, 10);
    }
    BS_TEST_VERIFY_EQ(test_ptr, 1, done);
    int x, y;
    wlmtk_element_get_position(&fe_ptr->element, &x, &y);
    BS_TEST_VERIFY_EQ(test_ptr, 300, x);
    BS_TEST_VERIFY_EQ(test_ptr, 200, y);
    BS_TEST_VERIFY_FALSE(test_ptr, wlmtk_effects_is_animating(&fe_ptr->element));

    wlmtk_element_destroy(&fe_ptr->element);
    // Reset, and drop the timer before the loop goes.
    wlmtk_effects_t off = { .active_opacity = 1.0, .inactive_opacity = 1.0 };
    wlmtk_effects_set(&off, NULL);
    if (NULL != _wlmtk_effects_timer_ptr) {
        wl_event_source_remove(_wlmtk_effects_timer_ptr);
        _wlmtk_effects_timer_ptr = NULL;
    }
    wl_event_loop_destroy(loop_ptr);
}

/* == End of effects.c ===================================================== */

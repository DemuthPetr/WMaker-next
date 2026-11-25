/* ========================================================================= */
/**
 * @file effects.h
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
#ifndef __WLMTK_EFFECTS_H__
#define __WLMTK_EFFECTS_H__

#include <libbase/libbase.h>
#include <stdbool.h>

#include "element.h"

struct wl_event_loop;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** Look & feel effects. All off (opaque, no animations) by default. */
typedef struct {
    /** Opacity of the activated window, 0.1 .. 1.0. */
    double                    active_opacity;
    /** Opacity of other windows, 0.1 .. 1.0. */
    double                    inactive_opacity;
    /** Whether minimizing and restoring fade and slide the window. */
    bool                      animate_minimize;
    /** Whether newly mapped windows fade in. */
    bool                      animate_map;
    /** Duration of animations, in milliseconds. */
    int                       duration_ms;
} wlmtk_effects_t;

/**
 * Sets the effects. Animations need the event loop, for their timer.
 *
 * @param effects_ptr
 * @param wl_event_loop_ptr   May be NULL: No animations then.
 */
void wlmtk_effects_set(
    const wlmtk_effects_t *effects_ptr,
    struct wl_event_loop *wl_event_loop_ptr);

/** @return The current effects. */
const wlmtk_effects_t *wlmtk_effects_get(void);

/** @return whether animations are possible and of `duration_ms` > 0. */
bool wlmtk_effects_can_animate(void);

/**
 * Applies `opacity` to every buffer drawn by the element (and children).
 *
 * @param element_ptr
 * @param opacity
 */
void wlmtk_effects_apply_opacity(wlmtk_element_t *element_ptr, double opacity);

/** Callback once an animation completed. */
typedef void (*wlmtk_effects_done_fn)(wlmtk_element_t *element_ptr,
                                      void *ud_ptr);

/**
 * Animates the element's opacity and a vertical offset, over the configured
 * duration. The offset is added to the element's position, which may still
 * change meanwhile (eg. a menu placed at the pointer). Replaces an animation
 * running for the same element. At the end, the offset is removed again and
 * `done_fn` is called.
 *
 * @param element_ptr
 * @param from_opacity
 * @param to_opacity
 * @param from_dy
 * @param to_dy
 * @param done_fn             May be NULL.
 * @param ud_ptr
 *
 * @return true if animating; false if not possible (nothing was done).
 */
bool wlmtk_effects_animate(
    wlmtk_element_t *element_ptr,
    double from_opacity,
    double to_opacity,
    int from_dy,
    int to_dy,
    wlmtk_effects_done_fn done_fn,
    void *ud_ptr);

/**
 * Cancels an animation of the element, if any: Removes its offset. Does not
 * call the `done_fn`.
 *
 * @param element_ptr
 *
 * @return whether an animation was cancelled.
 */
bool wlmtk_effects_cancel(wlmtk_element_t *element_ptr);

/** @return whether the element is animating. */
bool wlmtk_effects_is_animating(wlmtk_element_t *element_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmtk_effects_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMTK_EFFECTS_H__ */
/* == End of effects.h ===================================================== */

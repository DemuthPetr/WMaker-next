/* ========================================================================= */
/**
 * @file popup_menu.h
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
#ifndef __POPUP_MENU_H__
#define __POPUP_MENU_H__

#include <stdbool.h>
#include <stddef.h>

/** Forward declaration: A context menu, opened at the pointer. */
typedef struct _wlmaker_popup_menu_t wlmaker_popup_menu_t;

#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** An item of a popup menu. */
typedef struct {
    /** Text. */
    const char                *text_ptr;
    /** Passed to the callback when the item is triggered. */
    int                       id;
    /** Whether it can be triggered. */
    bool                      enabled;
} wlmaker_popup_menu_item_t;

/** Called when an item was triggered, after the menu closed. */
typedef void (*wlmaker_popup_menu_callback_t)(void *ud_ptr, int id);

/** Creates the popup menu (one, shared: There's one pointer). */
wlmaker_popup_menu_t *wlmaker_popup_menu_create(wlmaker_server_t *server_ptr);

/** Destroys the popup menu. */
void wlmaker_popup_menu_destroy(wlmaker_popup_menu_t *popup_menu_ptr);

/**
 * Opens the menu at the pointer, with these items; replaces an open one.
 *
 * @param popup_menu_ptr
 * @param title_ptr           Window title.
 * @param items_ptr
 * @param n                   Number of items.
 * @param callback            Called with `ud_ptr` & the item's id.
 * @param ud_ptr
 */
void wlmaker_popup_menu_open(
    wlmaker_popup_menu_t *popup_menu_ptr,
    const char *title_ptr,
    const wlmaker_popup_menu_item_t *items_ptr,
    size_t n,
    wlmaker_popup_menu_callback_t callback,
    void *ud_ptr);

/**
 * Closes the menu if it was opened for `ud_ptr`, without calling back. For
 * owners going away (eg. a launcher, when the dock is re-created).
 *
 * @param popup_menu_ptr
 * @param ud_ptr
 */
void wlmaker_popup_menu_cancel(
    wlmaker_popup_menu_t *popup_menu_ptr,
    void *ud_ptr);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __POPUP_MENU_H__ */
/* == End of popup_menu.h ================================================== */

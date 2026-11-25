/* ========================================================================= */
/**
 * @file window_list.h
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
#ifndef __WINDOW_LIST_H__
#define __WINDOW_LIST_H__

/** Forward declaration: Window list menu handle. */
typedef struct _wlmaker_window_list_t wlmaker_window_list_t;

#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Creates the window list menu: Lists the windows of all workspaces, as
 * Window Maker's middle-click menu. Selecting one switches to its workspace,
 * restores it if minimized, raises and activates it.
 *
 * @param server_ptr
 *
 * @return The window list, or NULL on error. Must be destroyed by calling
 *     @ref wlmaker_window_list_destroy.
 */
wlmaker_window_list_t *wlmaker_window_list_create(wlmaker_server_t *server_ptr);

/**
 * Destroys the window list.
 *
 * @param window_list_ptr
 */
void wlmaker_window_list_destroy(wlmaker_window_list_t *window_list_ptr);

/**
 * Opens the window list at the pointer position, with current contents.
 * Does nothing if it is already open.
 *
 * @param window_list_ptr
 */
void wlmaker_window_list_open(wlmaker_window_list_t *window_list_ptr);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WINDOW_LIST_H__ */
/* == End of window_list.h ================================================= */

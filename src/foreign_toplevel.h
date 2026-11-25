/* ========================================================================= */
/**
 * @file foreign_toplevel.h
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
#ifndef __FOREIGN_TOPLEVEL_H__
#define __FOREIGN_TOPLEVEL_H__

/** Forward declaration: Foreign toplevel manager handle. */
typedef struct _wlmaker_foreign_toplevel_t wlmaker_foreign_toplevel_t;

#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Implements wlr-foreign-toplevel-management-unstable-v1: Publishes the
 * client windows of all workspaces (title, app ID, state), and executes
 * requests from taskbars to activate, minimize, maximize or close them.
 *
 * @param server_ptr
 *
 * @return The handle, or NULL on error. Destroy by calling
 *     @ref wlmaker_foreign_toplevel_destroy.
 */
wlmaker_foreign_toplevel_t *wlmaker_foreign_toplevel_create(
    wlmaker_server_t *server_ptr);

/**
 * Destroys the foreign toplevel manager.
 *
 * @param foreign_toplevel_ptr
 */
void wlmaker_foreign_toplevel_destroy(
    wlmaker_foreign_toplevel_t *foreign_toplevel_ptr);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __FOREIGN_TOPLEVEL_H__ */
/* == End of foreign_toplevel.h ============================================ */

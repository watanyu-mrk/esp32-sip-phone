/*
 * Embedded web configuration UI and REST API.
 *
 * Every route (static assets and /api) requires HTTP Basic authentication with
 * the admin credentials stored in config_manager. Passwords are never returned
 * by the API; the UI only reports whether one is set.
 *
 * The UI is served from the repository's web/ directory, embedded into the app.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);

/* Registers the /api handlers on a running server (called by start). */
esp_err_t web_api_init(httpd_handle_t server);

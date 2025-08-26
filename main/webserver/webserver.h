#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <stdint.h>
#include <esp_err.h>

// Initialize the web server
esp_err_t webserver_init(void);

// Publish the latest distance value to be served by the web server
void webserver_publish_distance(uint32_t distance);

#endif // WEBSERVER_H

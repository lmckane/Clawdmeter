#pragma once
#include <stddef.h>

void c6_wifi_init(void);
void c6_wifi_tick(void);
void c6_wifi_start_portal(void);
void c6_wifi_stop_portal(void);
bool c6_wifi_portal_active(void);
void c6_wifi_status(char* out, size_t capacity);

#ifndef WIFI_H
#define WIFI_H

// Connects to the configured WiFi network (STA mode). Blocks until connected,
// or until WIFI_MAX_RETRY attempts have failed.
void wifi_init_sta(void);

#endif

#ifndef OTA_H
#define OTA_H

#include <stdbool.h>

// Kicks off a background task that downloads the firmware image at `url`
// (plain HTTP, same server the device already talks to) via esp_https_ota,
// and on success reboots into it. Safe to call from command_task's read
// loop -- returns immediately, does not block on the download.
void ota_start_update(const char *url);

// Compares dotted "x.y.z" version strings numerically segment-by-segment
// (not lexicographically -- "0.1.10" is newer than "0.1.9"). Missing
// segments are treated as 0. Returns true only if candidate is strictly
// newer than current_version.
bool ota_is_newer_version(const char *candidate, const char *current_version);

#endif // OTA_H

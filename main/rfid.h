#ifndef RFID_H
#define RFID_H

#include <stdint.h>

// Call once at boot (from app_main). Reads the RFID_RX pin from NVS, same as
// before, and starts the reader task only if one is configured -- if none is,
// this just logs and returns, same as the old auto-exiting rfid_task did.
void rfid_init(void);

// Call whenever a live pin config change (ChangePinConfig/ReplaceController)
// may have added, moved, or removed the RFID_RX pin -- new_pin is the GPIO
// found in the new config, or 0 if none. Starts the reader if it wasn't
// running, repoints an already-running one at the new GPIO, or stops it if
// RFID_RX was removed -- all without requiring a reboot.
void rfid_reinit(uint32_t new_pin);

// Returns the GPIO currently in use as the RFID reader's UART RX pin, or 0 if
// none is configured.
uint32_t rfid_get_rx_pin(void);

#endif // RFID_H

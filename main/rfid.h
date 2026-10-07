#ifndef RFID_H
#define RFID_H

#include <stdint.h>

void rfid_task(void *pvParameters);

// Returns the GPIO currently in use as the RFID reader's UART RX pin (as
// discovered from NVS at rfid_task startup), or 0 if no RFID_RX pin is
// configured for this controller.
uint32_t rfid_get_rx_pin(void);

#endif // RFID_H

#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

// FreeRTOS task entry point: connects to the ControlHome server and runs
// the device's main communication loop (register once per connection,
// then periodic heartbeat). Reconnects automatically on any socket error.
void device_task(void *pvParameters);
bool tcp_client_send_rfid(const char *rfid);
#endif

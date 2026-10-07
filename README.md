# IoT ESP32 Home Control — Controller Firmware

ESP-IDF firmware for an ESP32-S2 based home automation controller. The device
talks to a companion ASP.NET Core server over a raw TCP socket (Protobuf
framing, nanopb on this side) to report GPIO/RFID state and receive commands,
and it provisions itself over a local WiFi access point the first time it's
powered on — no hardcoded WiFi credentials or pin wiring in the firmware.

Server-side counterpart (ASP.NET Core + MariaDB) lives in a separate repo and
is not part of this project.

## Architecture

```
 ┌─────────────────────────┐        WiFi (AP mode, first boot only)
 │   Phone / laptop browser │ ───────────────────────────────┐
 └─────────────────────────┘                                 │
                                                               ▼
                                                   ┌───────────────────────┐
                                                   │   ESP32-S2 controller  │
                                                   │                        │
                                                   │  ap_mode.c   – HTTP    │
                                                   │    config server + NVS│
                                                   │  wifi.c      – STA     │
                                                   │    connect/retry       │
                                                   │  tcp_client.c– register│
                                                   │    /IoReport/Command   │
                                                   │  gpio.c      – pin     │
                                                   │    init/read/write     │
                                                   │  rfid.c      – UART    │
                                                   │    tag reader          │
                                                   └───────────┬────────────┘
                                                               │ WiFi (STA), TCP:9000
                                                               │ Protobuf (controlhome.proto)
                                                               ▼
                                                   ┌───────────────────────┐
                                                   │  ASP.NET Core server   │
                                                   │  (separate repo)       │
                                                   │  + MariaDB             │
                                                   └───────────────────────┘
```

## Hardware

Developed and tested on a **Lolin S2 Mini** (ESP32-S2, 2MB flash). Any
ESP32-S2 board should work; GPIO numbers are configured per-device at
provisioning time (see below), not hardcoded.

## First-time setup (AP provisioning mode)

On boot, the device checks NVS for a stored WiFi SSID. If none is found, it
starts its own access point instead of connecting to anything:

1. Power on the device. It creates a WiFi network named
   **`HomeControl-Setup-XXXX`** (last two bytes of its AP MAC address),
   password **`temp12345`**.
2. Connect a phone or laptop to that network.
3. Open `http://192.168.4.1` in a browser (use the browser app directly if a
   captive-portal mini-browser pops up and JavaScript doesn't run).
4. Fill in the form:
   - **WiFi SSID / password** — the real network the controller should join.
   - **Mode**, pick one:
     - **New controller** — enter a controller ID and description, then
       assign a function to each available GPIO pin from the dropdowns
       (see Pin configuration below).
     - **Replace existing controller** — enter the ID of the controller
       being replaced. Pin configuration is fetched automatically from the
       server after registration; nothing to fill in by hand.
5. Submit. The device saves everything to NVS and reboots into normal
   (STA) operation, connects to the real WiFi network, and registers itself
   with the server using the saved configuration.

If WiFi authentication fails repeatedly (bad password, AP not found), the
device clears its stored credentials and automatically falls back to AP mode
again on the next boot.

## Pin configuration

Internally, pin assignments are stored in NVS as a compact string:

```
gpio:FUNCTION,gpio:FUNCTION,...
```

e.g. `4:GPIO_IN,5:GPIO_IN,10:GPIO_OUT,33:I2C_SDA,37:RFID_RX`

Supported functions (selectable per-pin in the provisioning form):

| Function     | Meaning                                                    |
|--------------|-------------------------------------------------------------|
| `GPIO_IN`    | Digital input, reported in every `IoReport`                 |
| `GPIO_OUT`   | Digital output, drivable by server `Command` messages        |
| `SPI_SCK/MISO/MOSI/SS` | Reserved for a SPI peripheral (not GPIO-managed)    |
| `I2C_SDA/SCL`| Reserved for an I2C peripheral (not GPIO-managed)            |
| `RFID_RX`    | UART RX pin for the onboard RFID reader (one supported)      |

Only one `RFID_RX` pin is supported at a time; `rfid_task` reads it from NVS
at startup and exits quietly if none is configured. `SPI_*`/`I2C_*`/`RFID_RX`
pins are intentionally left out of `gpio_ctrl_init()`'s managed input/output
lists — their owning peripheral driver (UART, SPI, I2C) configures them
directly instead.

The full list of pins offered in the provisioning UI, and which functions
each one supports, is defined in [`main/html/pins.json`](main/html/pins.json).

## Server connection

The server address is currently compiled in (`SERVER_IP`/`SERVER_PORT` in
[`main/tcp_client.c`](main/tcp_client.c)) — update it for your deployment
before building. The wire protocol (`Envelope`, `RegisterController`,
`IoReport`, `Command`, etc.) is defined in
[`main/proto/controlhome.proto`](main/proto/controlhome.proto), shared with
the server's copy of the same schema.

## Building and flashing

Requires ESP-IDF v5.4.x.

```
idf.py set-target esp32s2
idf.py build
idf.py -p <PORT> flash monitor
```

## Project structure

```
├── CMakeLists.txt
├── main/
│   ├── IoT_generic_main.c   app_main: NVS init, AP-mode/STA branch, task spawn
│   ├── ap_mode.c / .h       First-boot HTTP provisioning server (AP + NVS)
│   ├── wifi.c / .h          STA connection, retry/backoff, auth-fail recovery
│   ├── tcp_client.c / .h    Register/Replace/IoReport/Command over TCP+Protobuf
│   ├── gpio.c / .h          GPIO init from stored config, read/write, ISR notify
│   ├── rfid.c / .h          UART RFID tag reader
│   ├── proto/               controlhome.proto + generated nanopb bindings
│   └── html/                 ap_config.html (provisioning form) + pins.json
└── README.md
```

## Known limitations / planned work

- Pin functions beyond `GPIO_IN`/`GPIO_OUT`/`SPI`/`I2C`/`RFID_RX` (PWM, ADC,
  DAC, 1-Wire) are defined in the data model but not yet wired up end-to-end.
- AP mode password is a fixed placeholder; a per-device HMAC-derived password
  is planned.
- No remote/server-triggered re-entry into AP mode yet (physical access to
  the device is currently required to reconfigure it).

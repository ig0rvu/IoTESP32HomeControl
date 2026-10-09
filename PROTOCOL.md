# Wire protocol (controlhome.proto)

Raw TCP (port defined by `SERVER_PORT` in `main/tcp_client.c`), framed messages,
no TLS. Every message in both directions is:

```
[4-byte big-endian length][serialized Envelope bytes]
```

The length prefix covers only the serialized `Envelope` payload, not itself.

## Schema location — kept in sync manually

The same schema is duplicated in two repos and must be edited in both places
by hand:

- `main/proto/controlhome.proto` — this repo (ESP32 firmware, nanopb)
- `Server/Protos/controlhome.proto` — server repo (C#, Google.Protobuf)

There is no shared package/submodule linking them. After changing one, copy
the change to the other and regenerate bindings on both sides (see below for
the ESP32 side; the C# side regenerates automatically on build via its
`.csproj` Protobuf item).

## Message overview

`Envelope` is the top-level frame; `mac_address` is always set, and exactly
one of the `oneof payload` fields is populated:

| Message              | Direction        | Purpose                                                        |
|----------------------|-------------------|------------------------------------------------------------------|
| `Heartbeat`          | device → server  | Slow liveness fallback (every 5 min) when nothing has changed   |
| `RegisterController` | device → server  | First-time registration: id, description, pin list              |
| `ReplaceController`  | device → server  | Bind a new MAC to an existing `idcontroller`                     |
| `ControllerConfig`   | server → device  | Reply to `ReplaceController`: the pin list to adopt              |
| `IoReport`           | device → server  | Digital/analog pin state report                                  |
| `Command`            | server → device  | Drive one output pin (gpio, action, duration)                    |
| `Rfid`               | device → server  | RFID tag read event                                              |
| `RequestStatus`      | server → device  | Ask for an immediate `IoReport` (e.g. on dashboard refresh)      |
| `Ack`                | server → device  | Generic status_code + message reply                             |

`IoReport` is event-driven, not polled on a fixed interval: a device sends one
right after it (re)connects (baseline state), whenever a monitored pin changes
(GPIO interrupt wakes the send loop), or immediately on receiving
`RequestStatus`. If neither happens for 5 minutes, `Heartbeat` goes out
instead, just to prove the device is still alive without resending full pin
state.

## Why there's a `.options` file

nanopb generates fixed-size, statically-allocated C structs (no malloc), so
every `string`/`repeated` field needs an explicit max size/count —
`main/proto/controlhome.options` is that config, e.g.:

```
controlhome.RegisterController.pins max_count:20
controlhome.Ack.message max_size:256
```

If you add a new `string`/`repeated` field to the `.proto`, add a matching
line here or generation will fail (or silently truncate — nanopb warns at
generation time if a size isn't specified and a default is assumed).

## Regenerating the ESP32-side bindings after a `.proto` change

`main/proto/controlhome.pb.c` / `.h` are generated and checked into this repo
so the project builds without extra tooling for day-to-day work. The
generator itself (`nanopb_generator.exe`) is **not** checked in
(`.gitignore`) — it's a third-party binary, not project source.

1. Get the generator: download the matching nanopb release for your OS from
   https://github.com/nanopb/nanopb/releases (the `generator-<version>-windows-x86.zip`
   asset bundles `nanopb_generator.exe`, no separate Python/protoc install
   needed). Drop it into `main/proto/` (ignored by git) or anywhere on PATH.
2. Edit `main/proto/controlhome.proto` (and the matching copy in the server
   repo).
3. From `main/proto/`, run:
   ```
   nanopb_generator.exe controlhome.proto
   ```
   It picks up `controlhome.options` automatically (same basename,
   colocated) and overwrites `controlhome.pb.c` / `controlhome.pb.h` in
   place.
4. Rebuild (`idf.py build`) and fix any call sites touched by the schema
   change (field renames, new required max_size entries, etc.).

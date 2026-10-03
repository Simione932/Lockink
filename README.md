# lockink-cli

C command-line tool that exercises LockInk AA-A10xx BLE devices using the
**SimpleBLE** C library (`simplecble`), cross-platform (macOS / Windows / Linux).

The protocol was reverse-engineered — 'lockink-protocol.md` for the full byte-level reference.

## Build

Requires `simpleble`. Builds two binaries:

- `lockink-cli` — the full command-line exerciser
- `lockink-diag` — minimal connect/GATT/subscribe diagnostic (isolates GATT issues)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
# -> build/bin/lockink-cli
```

Platform dependencies:
- **macOS**: Xcode command-line tools; links CoreBluetooth/IOBluetooth automatically.
- **Windows**: Visual Studio 2022 (CMake generator); WinRT BLE stack.
- **Linux**: `libdbus-1-dev` (and BlueZ running).

## Usage

```
lockink-cli <command> <device> [args]
  <device> = advertised name (e.g. AA-A1012) or MAC/persistent identifier

  scan [timeout_ms]                       list all scanned BLE devices
  find [timeout_ms]                       wait for a LockInk (AA-A10xx) device
  lock <dev>                              send lock frame
  unlock <dev>                            send unlock frame (AA-A1002: short frame)
  shock <dev> <freq> <voltage> <secs>     manual classic shock + keep-alive + auto-stop
  classic <dev> <sf> <si> <efreq> <volt> <repeat_ms> [duration_s]
  random <dev> <minV> <maxV>              random mode (AA-A1012 only)
  berserk <dev> <1|0>                     berserk mode (AA-A1012 only)
  stop <dev> [1]                          classic stop frame (+ optional legacy stop)
  battery <dev>                           sends battery query, waits for notification
```

Example:

```sh
./lockink-cli find
./lockink-cli unlock AA-A1012
./lockink-cli shock AA-A1012 4 100 15
./lockink-cli battery AA-A1012
```

## Notes

- Discovery matches the advertised name pattern `AA-A10XX` (as the app
  does). On macOS the reported "address" is the CoreBluetooth persistent
  identifier, not the MAC; on Linux/Windows it is the MAC.
- All writes use GATT **write-with-response** (the app's `writeBytesWithAck`);
  a 100 ms pre-write delay ("manufacturer pattern") is applied after connecting.
- While `shock` runs, a keep-alive pulse (`AA 09 06 00 00 04 01 00 00 FF` — a
  tiny shock) is sent every 5 s, mirroring the app's anti-auto-shutdown heartbeat.
- Device notifications are hex-dumped; battery reports (`data[2]==0x05`) are
  decoded to a percentage.
- Battery query frame: `AA 09 05 00 00 00 00 00 00 FF` (AA-A1002: `AA 03`);
  the device replies by echoing the frame with header byte 0xBB (verified live).

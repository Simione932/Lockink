# LockInk BLE Protocol — extracted from the Chastify app (`net.chastify.app`)

Sources: decompiled Kotlin (jadx, `classes3.dex`) + smali (baksmali) + web bundle (`assets/public`).
Everything below is verified against code, not guessed.

## 1. Architecture

The app is a **Capacitor hybrid**: web UI (React in `assets/public`) + Kotlin native layer.
Web ↔ native goes through the Capacitor bridge (`Capacitor.nativePromise(plugin, method, payload)`);
the native plugin is `NativeShock` (classes in `net.chastify.app`):

```
NativeShockService (foreground Service)
  └─ NativeShockRemoteSupport        (remote/socket commands, token refresh, safety stops)
      └─ NativeShockDeviceCommands    (lock/unlock, vibration, scheduled shocks)
          └─ NativeShockLockinkCommands (lockink modes: manual/classic/random/berserk, keep-alive)
              └─ NativeShockPearFlowerCommands
BLE plumbing: NativeShockBluetoothSupport (connectGatt, GattWriteTracker, notifications)
Discovery:     LockinkScanner + LockinkScanProtocol
Keep-alive:    LockinkKeepAliveRuntime / LockinkKeepAliveState
Stop recovery: LockinkStopRecovery
Catalog:       nativeshock.NativeShockDeviceCatalog
```

The web layer also has its own BLE client (Capacitor BLE plugin: `initialize/requestLEScan/connect/startNotifications/writeWithoutResponse`), used for pairing and status; the native service does the shock/lock frames.

## 2. Device discovery (BLE scan)

Devices advertise a **name** that must match:

| Advertised name | deviceType |
|---|---|
| `AA-A1001`, `AA-A1002`, `AA-A1003`, `AA-A1012` (regex `^AA-A(10\d{2})$`) | `lockink-aa-a1001` … `lockink-aa-a1012` |
| `YS03` / `YS04` | `yokonex-ys03` / `yokonex-ys04` |

Default when name is missing: `lockink-aa-a1012`. Scan callback emits
`{name, bluetoothAddress, rssi, deviceType}`. MAC = BLE address; YoKoNex MAC is also
extracted from scan-record bytes 6..16.

Display names: `lockink-aa-a1012` = **"Lockink Beesting"**; `lockink-aa-a10XX` = "Lockink AA-A10XX".

## 3. GATT UUIDs (authoritative — from `NativeShockDeviceCatalog`)

| Device family | Service | Listen (notify) | Write |
|---|---|---|---|
| **LockInk (AA-A)** | `00008ac0-0000-1000-8000-00805f9b34fb` | `00008ac2-...` | `00008ac1-...` |
| QIUI (pear flower) | `0000fee7-...` | `000036f6-...` | `000036f5-...` |
| Keypod 1 | `0000fff0-...` | `0000fff2-...` | `0000fff1-...` |
| Cagink metal | `0000fee5-...` | `00003ff6-...` | `00003ff5-...` |

(All share the `...-0000-1000-8000-00805f9b34fb` BT base.)

- Notifications enabled via CCCD `00002902-0000-1000-8000-00805f9b34fb`.
- Writes use **GATT write with response**; the write-response *is* the ack (`writeBytesWithAck(frame, timeoutMs)`, typically 1500 ms; `writeBytesNoWait` for lock/unlock).
- Note: the web pairing layer's fallback table uses the QIUI set (`fee7/36f6/36f5`) as the
  default and per-device profiles learned at pairing; the **native shock service uses the
  `8ac0/8ac1/8ac2` set for LockInk**.

## 4. Command frame format

All LockInk frames are 10 bytes:

```
[0xAA, 0x09, method, b1, b2, b3, b4, b5, b6, 0xFF]
        ^len=9      ^method  ^payload^   ^crc/end
```

Built by `LockinkDeviceController.frame(method, b1..b6)`.

### Method bytes (LockInk)

| method | Meaning | Builder |
|---|---|---|
| `0x01` | electric | `electricFrame(freq, enabled, x, y)` = `AA 09 01 freq en x y 00 00 FF` |
| `0x06` | classic (loop) | `classicFrame(shakeFreq, shakeIntensity, electricFreq, voltage)` = `AA 09 06 sf si ef v 00 00 FF`; 2-arg form = `classicFrame(electricFreq, voltage)` with sf=si=0 |
| `0x08` | berserk | `AA 09 08 <1\|0> 00 00 00 00 00 FF` (1=start, 0=stop) — **AA-A1012 only** |
| `0x09` | random | `AA 09 09 <1\|0> minV maxV 00 00 00 FF` (1=start, 0=stop) — **AA-A1012 only** |

Extras:
- `electricStopFrame(freq)` = `AA 09 01 freq 00 00 01 00 00 FF`
- `electricZeroFrame()` = `AA 09 01 00 00 00 00 00 00 FF`
- `legacyElectricStopFrame` = `AA 02 00 01 00 00` (6-byte legacy stop)

### Random-mode voltage scaling
`randomModeCommand(start, minV, maxV, is100Scale)`: values clamped to 1..100 for AA-A1012
(`is100Scale`), 0..255 otherwise, then sorted so min ≤ max.

## 5. Lock / unlock

Web payload → native: `execute_native_lock` / `execute_native_unlock` with
`{bluetoothAddress | macAddress, serialNumber, deviceType, callNumber}`.

Native `handleExecuteNativeLock/Unlock` → `executeLockUnlockCommand(address, serial, deviceType, "lock"|"unlock", callNumber)`:

1. If not already GATT-connected, `connectGatt(address, deviceType)` (fails → "BLE connect failed").
2. Gate: `NativeShockDeviceCatalog.supportsLockUnlock(deviceType)` — true for
   `lockink-aa-a*`, pear flower, QIUI family, YoKoNex.
3. **LockInk AA-A1012 / 1001 / 1003** (built locally, written no-wait):

   | Command | Frame |
   |---|---|
   | **lock** | `AA 09 03 00 04 05 06 07 08 FF` |
   | **unlock** | `AA 09 02 00 04 05 06 07 08 FF` |

4. **AA-A1002**: `lock` is **ignored** ("AA-A1002 lock ignored"); `unlock` writes `AA 01 01 00` (4 bytes).
5. **YoKoNex**: separate path (`executeYokonexLockUnlockCommand`).
6. **Non-LockInk devices** (QIUI/Keypod/Cagink): command is fetched via a refreshed token /
   backend channel (`getOrRefreshToken` → "Token command missing" on failure) — not local frames.

## 6. Shocking

### Manual shock (`lockink_manual_shock`)
Payload: `{voltage=100 (default), frequency=1 (default), duration=10 s (clamped 1..600),
isLongShock, force, isLiveUpdate, callNumber, deviceType}`.

Per-model clamps:
- voltage: max **100** for AA-A1001/1003/1012, max **255** otherwise.
- frequency: max **10** (1012), **3** (1001), **1** (1003); min 1.
- "long shock" = `isLongShock || duration > 10`.
- AA-A1001/1003 have a post-cancel cooldown ("Shock device is cooling down").

Frames: classic frame with clamped freq/voltage (`classicFrame(...)`), written with ack.
AA-A1012 **live update** while a manual shock is active rewrites
`classicFrame(freq, 0/1)` (frequency change / long flag) instead of restarting.
Auto-stop is scheduled after `duration` seconds (`"Executing Lockink auto-stop after Ns"`),
via an exact alarm (`ACTION_LOCKINK_KEEP_ALIVE_ALARM`) + watchdog.

### Classic mode (`lockink_classic_mode`)
Payload: `{shakeFrequency=0, shakeIntensity=0, voltage=0, electricFrequency=1,
duration ≤ 600 s, repeatMs=250 (100..1000)}`.
Start/update frame = `classicFrame(sf, si, electricFreq, voltage)`; the loop re-sends it
every `repeatMs`; **stop = `classicFrame(0,0)` = `AA 09 06 00 00 00 00 00 00 FF`**.

### Random / berserk modes
- Random: start = `AA 09 09 01 minV maxV 00 00 00 FF`, stop = same with `00`.
- Berserk: start = `AA 09 08 01 00 00 00 00 00 FF`, stop with `00`.
- Both **AA-A1012 only** (enforced natively: "Berserker mode only supported on Lockink AA-A1012").

### Cancel / stop
`cancel_lockink_all_shocks` → "All Lockink shocks cancelled"; `stop_connected_lockink_modes`.
Stops are confirmed via `LockinkStopRecovery`: if a stop write isn't acked, the app
reconnects and re-sends until confirmed ("Pending Lockink stop could not be confirmed after reconnect").

### Safety gating
Each shock goes through `evaluateShockSafety`, a shock journal, `callNumber` dedup
("Duplicate execute_native_shock suppressed"), and "newer safety stop" rules
("execute_native_shock blocked by a newer safety stop"). Parental-control and
tamper-protection checks gate everything.

## 7. Keep-alive / heartbeat (the tiny shock)

While any shock mode is active, a tick runs **every 5000 ms** (`0x1388` ms):

1. **Pulse frame = `classicFrame(4,1)` = `AA 09 06 00 00 04 01 00 00 FF`** —
   classic method with electricFrequency=**4**, voltage=**1** → a minimal, very small shock.
2. Pause (~1 s), then restore the original active shock frame (byte[3] zeroed).

Purpose: the device **auto-stops after ~3 minutes** of inactivity, so this micro-shock
keeps the "shock active" state alive between real shocks. User-toggleable
(`lockinkKeepAliveEnabled` in web settings). Related constants:
`LOCKINK_KEEPALIVE_MS`, `LOCKINK_HEARTBEAT_MS`, `LOCKINK_MODE_PING_MS`,
`LOCKINK_KEEP_ALIVE_WATCHDOG_MS`, `LOCKINK_POST_MODE_KEEP_ALIVE_DELAY_MS`.

## 8. Notifications from the device (listen characteristic)

Frame: `[_, _, cmd, payload, ...]`. Parsed in the GATT callback:

- **Battery**: `value[2] == 0x05` → `value[3]` = battery percent (0..100), persisted and
  broadcast. (Standard `BATTERY_SERVICE_UUID`/`BATTERY_LEVEL_UUID` also used for other devices.)
- Initial battery query shortly after connect (`LOCKINK_INITIAL_BATTERY_DELAY_MS`),
  retried on timeout (`LOCKINK_RETRY_BATTERY_DELAY_MS`, `lockinkConsecutiveBatteryTimeouts`),
  plus periodic heartbeat queries.

## 9. Minimal implementation sketch (what you need to replicate)

```
1. BLE scan for advertised names matching ^AA-A(10\d{2})$  (or YS0[34]).
2. GATT connect to the address.
3. Discover service 00008ac0-0000-1000-8000-00805f9b34fb.
4. Enable notifications on 00008ac2-... via CCCD 00002902-....
5. Write with response to 00008ac1-...:
     lock:   AA 09 03 00 04 05 06 07 08 FF
     unlock: AA 09 02 00 04 05 06 07 08 FF
     (AA-A1002: lock n/a, unlock AA 01 01 00)
6. Shock (classic, AA-A10xx):
     start/update: AA 09 06 00 00 <freq> <voltage> 00 00 FF   (freq 1..10 on 1012; volt 0..100)
     stop:         AA 09 06 00 00 00 00 00 00 00 FF
     keep-alive tick every 5 s: AA 09 06 00 00 04 01 00 00 FF  (tiny shock)
     electric stop (legacy):   AA 02 00 01 00 00
7. Random (1012): AA 09 09 01 <minV> <maxV> 00 00 00 FF / stop = 09 00 ...
   Berserk (1012): AA 09 08 01 00 00 00 00 00 FF / stop = 08 00 ...
8. Battery: write AA 09 05 00 00 00 00 00 00 FF, read reply bytes[2]==0x05 → bytes[3]=%.
   (device echoes the frame with header 0xBB)
```

### Caveats
- Frames 5/6/7/8 in the lock command (`04 05 06 07 08`) look like literal placeholder bytes
  in the firmware protocol; treat them as fixed bytes (they're constants in the bytecode, not computed).
- The app wraps every frame in safety state (wake locks, stop-recovery, dedup); a bare
  implementation should at minimum: stop on disconnect, confirm stops, and not restart a
  stop that already succeeded.
- Non-AA-A devices (QIUI/Keypod/Cagink) get their command bytes from the Chastify backend
  (token/MQTT path), so reverse-engineering those requires the API side, not just BLE.

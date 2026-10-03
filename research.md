# Chastify App — LockInk Device Support Research

**Source:** `net.chastify.app` APK (133,971,226 bytes) pulled via `adb -s 5d2a9a4d pull /data/app/net.chastify.app-1/base.apk` into `/Users/paul/temp/chastify.apk`, unpacked to `/Users/paul/temp/chastify_extract/`.

## TL;DR

Yes — the Chastify app has deep, first-class support for **LockInk** devices. LockInk is treated as a native companion "lock" product line (sibling brand to **QIUI** in the same chastity-lock ecosystem) with dedicated native BLE code, a web-UI settings layer, per-device shock profiles, keep-alive/heartbeat management, battery monitoring, tamper protection, and an accessibility-based "app blocker."

## Device models found

Detected by `brand === 'lockink'` or `deviceType` starting with `lockink-` (web layer, `feature-locks-*.js` / `feature-device-control-*.js`).

| deviceType | Display name (in-app) | Notable support |
|---|---|---|
| `lockink-aa-a1012` | **Lockink Beesting** / "Eel Sting" | Full feature set: classic mode, random mode, **berserk mode (1012 only)**, manual shock, keep-alive |
| `lockink-aa-a1001` | **Lockink Smart Sub** | Classic/manual shock, keep-alive |
| `lockink-aa-a1003` | (AA-A variant) | Classic/manual shock |
| `lockink-aa-a1002` | (AA-A variant) | Present in native dex constants (not in the web JS model set) |
| `lockink-blackbox` | **Lockink Blackbox** | Companion "key" hardware; pairs with QIUI Keypod (UI: "Lockink Blackbox / QIUI Keypod") |

- Native string: `"Berserker mode only supported on Lockink AA-A1012. Device="` → mode gating is enforced in code, not just UI.
- Localized string (de catalog): *"Zufallsmodus nur auf Lockink AA-A1012 unterstützt"* (random mode only on AA-A1012).
- Native model constants: `lockink-`, `lockink-aa-a1001`, `lockink-aa-a1002`, `lockink-aa-a1003`, `lockink-aa-a1012`; web set: `{lockink-aa-a1012, lockink-aa-a1001, lockink-aa-a1003}`.

## How it connects (BLE)

- **Permissions** (AndroidManifest.xml): `BLUETOOTH`, `BLUETOOTH_ADMIN`, `BLUETOOTH_ADVERTISE`, `BLUETOOTH_CONNECT`, `BLUETOOTH_SCAN`, `FOREGROUND_SERVICE_CONNECTED_DEVICE`, `WAKE_LOCK`, `SCHEDULE_EXACT_ALARM`, `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`, `BIND_ACCESSIBILITY_SERVICE`.
- **GATT symbols** in `classes3.dex`: `LOCKINK_SERVICE_UUID`, `LOCKINK_LISTEN_UUID`, `LOCKINK_WRITE_UUID` (dedicated LockInk service + listen/write characteristics), plus `BATTERY_SERVICE_UUID` / `BATTERY_LEVEL_UUID` (standard battery service for battery queries). Sibling-device UUID families also present: `CAGINK_*`, `KEYPOD_*` (QIUI), `LOVENSE_NORDIC_RX_UUID`.
- **Web Bluetooth**: the web layer filters the device list by `webBluetoothId` — pairing happens through the in-app web UI (Web Bluetooth API), then native code drives the shock protocol.
- **Native BLE pipeline** (`classes3.dex` classes): `LockinkScanner` (+ `StartResult`, `Listener`), `LockinkScanProtocol`, `LockinkDeviceController`, `NativeShockService` (+ `NativeShockLockinkCommands`, `LockinkKeepAliveAlarmReceiver`), `LockinkKeepAliveRuntime`, `LockinkStopRecovery`.
- Discovery uses a name pattern constant (`LOCKINK_NAME_PATTERN`) with a scan timeout (`DEFAULT_LOCKINK_SCAN_TIMEOUT_MS`).

## Shock & mode control

Native command verbs (dex strings): `execute_lockink_manual_shock`, `execute_lockink_classic_mode`, `execute_lockink_random_mode`, `execute_lockink_berserk_mode`, `cancel_lockink_all_shocks`.

Per-device shock settings stored in `lockinkShockSettings` keyed by deviceType, payload:
`{voltage, frequency, durationSeconds, allowedFrequencies, allowRandomMode, allowBerserkMode}` plus:
- `lockinkVoltageCap`, `lockinkVoltageMin`/`lockinkVoltage` (sliders; intensity + frequency presets),
- `keyholderShockPolicy`: `allow_max` | `custom` (per-device `customLockinkVoltage`) | `mirror_user`.

## Reliability / state management (native side)

- **Keep-alive/heartbeat**: `LOCKINK_KEEPALIVE_MS`, `LOCKINK_HEARTBEAT_MS`, `LOCKINK_MODE_PING_MS`, `LOCKINK_KEEP_ALIVE_WATCHDOG_MS`, `LOCKINK_POST_MODE_KEEP_ALIVE_DELAY_MS`, alarm receiver (`ACTION_LOCKINK_KEEP_ALIVE_ALARM`, tag `LOCKINK_BEESTING_KEEP_ALIVE_WORK_TAG`), foreground service labels `Chastify:LockinkKeepAlive` / `Chastify:LockinkAlarmHandoff`.
- **Auto-stop + recovery**: `"Executing Lockink auto-stop after ..."`, `"Pending Lockink stop could not be confirmed after reconnect"`, `LockinkStopRecovery` class — the app actively ensures shocks terminate and re-sends stop commands on disconnect/reconnect.
- **Battery monitoring**: initial/retry/heartbeat battery queries (`LOCKINK_INITIAL_BATTERY_DELAY_MS`, `LOCKINK_RETRY_BATTERY_DELAY_MS`, `lockinkConsecutiveBatteryTimeouts`, "battery query failed" diagnostics).

## Tamper protection & app blocking

- `LockinkTamperSetupActivity` (setup flow), `setLockinkTamperProtectionEnabled`, `[Native] setLockinkTamperProtectionTemporaryBypass`, "Disable Lockink protection first" — tamper protection gates shock features.
- `LockinkBlockAccessibilityService` (+ `LockinkTreeScanResult`) uses the accessibility service (`BIND_ACCESSIBILITY_SERVICE`) to block apps on the wearer's phone; status APIs `getLockinkAppBlockerStatus` / `getLockinkTamperProtectionStatus` expose `{supported, isAccessibilityEnabled, isServiceRunning, isBlockingEnabled}`.
- Related flags: `lockinkDeviceAdmin`, `lockinkOverlayPermission`, `chastify_lockink_block`, `chastify_lockink_tamper`.
- **Companion app**: package constant `com.lockink.phonelive` — the standalone LockInk PhoneLive app is referenced for app-blocker/device-admin interop.

## Ecosystem context

- QIUI is the sibling hardware brand (`QIUI Keypod`, `CAGINK_*`/`KEYPOD_*` UUIDs); lock configs can require `QIUI_ONLY`, `QIUI_AND_LOCKINK`, or `NONE` (`lockTypeRequirement`).
- UI strings: *"This lock requires either a QIUI or Lockink device."*, *"Ready to pair your Qiui / Lockink device?"*.
- Parental-control integration: `[Native][LockinkReport]` events (parental gating, deviceId reporting, report debounce).
- Localization: LockInk strings localized across ~19 translation catalogs (en/de/fr/es/it/pt-BR/ru/ja/ko/zh-Hans/pl/cs/sv/no/da/fi/hu/hr/hi, etc.).

## Conclusion

LockInk support is not a thin integration: it spans dedicated BLE GATT protocol classes, per-device shock profiles with safety caps, heartbeat/keep-alive/watchdog and stop-recovery machinery, battery telemetry, tamper-protection and accessibility-based app blocking, a companion app (`com.lockink.phonelive`), and full i18n coverage. Four AA-A battery-powered lock models are recognized natively (1001/1002/1003/1012) plus the Blackbox companion, with feature gating (berserk/random modes) restricted to the AA-A1012 "Beesting".

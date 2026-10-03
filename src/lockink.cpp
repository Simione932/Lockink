/*
 * lockink-cli — exercises LockInk AA-A10xx BLE devices (C++ / SimpleBLE).
 *
 * Protocol reverse-engineered from the Chastify app (net.chastify.app):
 *   frame format : AA 09 <method> b1 b2 b3 b4 b5 b6 FF   (10 bytes)
 *   lock         : AA 09 03 00 04 05 06 07 08 FF
 *   unlock       : AA 09 02 00 04 05 06 07 08 FF
 *   AA-A1002     : unlock = AA 01 01 00 (lock is unsupported)
 *   classic      : AA 09 06 <sf> <si> <ef> <volt> 00 00 FF  (stop = all zeros)
 *   random (1012): AA 09 09 <start> <minV> <maxV> 00 00 00 FF
 *   berserk(1012): AA 09 08 <start> 00 00 00 00 00 FF
 *   keep-alive   : classicFrame(4,1) every 5000 ms (tiny shock, keeps the
 *                  device out of its ~3 min auto-shutdown)
 *   battery      : notification with data[2]==0x05 -> data[3] = percent
 *
 * GATT (NativeShockDeviceCatalog):
 *   service 00008ac0-0000-1000-8000-00805f9b34fb
 *   write   00008ac1-0000-1000-8000-00805f9b34fb
 *   listen  00008ac2-0000-1000-8000-00805f9b34fb
 */

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <simpleble/SimpleBLE.h>

using namespace SimpleBLE;

static const char *LOCKINK_SERVICE_UUID = "00008ac0-0000-1000-8000-00805f9b34fb";
static const char *LOCKINK_WRITE_UUID = "00008ac1-0000-1000-8000-00805f9b34fb";
static const char *LOCKINK_LISTEN_UUID = "00008ac2-0000-1000-8000-00805f9b34fb";

static const int KEEPALIVE_MS = 5000;
static const int PRE_WRITE_DELAY_MS = 100; /* the app delays ~100 ms before writing */

static Adapter g_adapter;
static std::vector<Peripheral> g_results;

/* ------------------------------ helpers ------------------------------ */

static bool str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower(static_cast<unsigned char>(*a)) != tolower(static_cast<unsigned char>(*b))) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool uuid_ieq(const std::string &u, const char *str) { return str_ieq(u.c_str(), str); }

static void hex_print(const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02X", d[i]);
    printf("\n");
}

/* ^AA-A(10\d{2})$ case-insensitive */
static bool is_lockink_name(const std::string &name) {
    if (name.size() != 8) return false;
    if (!str_ieq(name.substr(0, 6).c_str(), "AA-A10")) return false;
    return name[6] >= '0' && name[6] <= '9' && name[7] >= '0' && name[7] <= '9';
}

/* ------------------------------ frame builders ------------------------------ */

static ByteArray frame10(int method, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5, uint8_t b6) {
    return ByteArray{uint8_t(0xAA), uint8_t(0x09), uint8_t(method), b1, b2, b3, b4, b5, b6, uint8_t(0xFF)};
}

static ByteArray frame_lock() { return frame10(3, 0, 4, 5, 6, 7, 8); }
static ByteArray frame_unlock() { return frame10(2, 0, 4, 5, 6, 7, 8); }
static ByteArray frame_a1002_unlock() { return ByteArray{uint8_t(0xAA), uint8_t(0x01), uint8_t(0x01), uint8_t(0x00)}; }
static ByteArray classic_frame(int electric_freq, int voltage) {
    return frame10(6, 0, 0, static_cast<uint8_t>(electric_freq), static_cast<uint8_t>(voltage), 0, 0);
}
static ByteArray classic_frame_full(int shake_freq, int shake_intensity, int electric_freq, int voltage) {
    return frame10(6, static_cast<uint8_t>(shake_freq), static_cast<uint8_t>(shake_intensity),
                   static_cast<uint8_t>(electric_freq), static_cast<uint8_t>(voltage), 0, 0);
}
static ByteArray classic_frame_stop() { return frame10(6, 0, 0, 0, 0, 0, 0); }
static ByteArray keepalive_frame() { return classic_frame(4, 1); }
static ByteArray battery_query_frame() { return frame10(5, 0, 0, 0, 0, 0, 0); }
static ByteArray battery_query_frame_a1002() { return ByteArray{uint8_t(0xAA), uint8_t(0x03)}; }
static ByteArray random_frame(int start, int min_v, int max_v) {
    if (min_v > max_v) std::swap(min_v, max_v);
    return frame10(9, static_cast<uint8_t>(start), static_cast<uint8_t>(min_v), static_cast<uint8_t>(max_v), 0, 0, 0);
}
static ByteArray berserk_frame(int start) { return frame10(8, static_cast<uint8_t>(start), 0, 0, 0, 0, 0); }
static ByteArray legacy_electric_stop() {
    return ByteArray{uint8_t(0xAA), uint8_t(0x02), uint8_t(0x00), uint8_t(0x01), uint8_t(0x00), uint8_t(0x00)};
}

/* ------------------------------ BLE plumbing ------------------------------ */

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/* Scan in 5 s chunks until a device matching the selector (name or address) appears. */
static bool scan_find(const std::string &selector, int timeout_ms) {
    g_results.clear();
    g_adapter.set_callback_on_scan_found([&](Peripheral p) {
        std::string name = p.identifier();
        std::string addr = p.address();
        printf("  found: %s [%s]\n", name.c_str(), addr.c_str());
        if (str_ieq(name.c_str(), selector.c_str()) || str_ieq(addr.c_str(), selector.c_str()))
            g_results.push_back(p);
    });
    int elapsed = 0;
    while (elapsed < timeout_ms && g_results.empty()) {
        g_adapter.scan_start();
        for (int i = 0; i < 5000 && g_results.empty(); i += 100) sleep_ms(100);
        g_adapter.scan_stop();
        elapsed += 5000;
    }
    return !g_results.empty();
}

static void print_scan_results() {
    for (size_t i = 0; i < g_results.size(); i++) {
        printf("[%zu] %-8s %s\n", i, g_results[i].identifier().c_str(), g_results[i].address().c_str());
    }
}

static void on_notify(ByteArray data) {
    printf("  NOTIFY %s: ", LOCKINK_LISTEN_UUID);
    hex_print(data.data(), data.size());
    /* battery notification: data[2] == 0x05, data[3] = percent (0..100) */
    if (data.size() >= 4 && data[2] == 0x05 && data[3] <= 100) {
        printf("  battery: %d%%\n", (int)data[3]);
    }
}

static bool connect_device(Peripheral &p) {
    printf("Connecting to %s [%s]\n", p.identifier().c_str(), p.address().c_str());
    try {
        p.connect();
    } catch (const std::exception &e) {
        printf("Failed to connect: %s\n", e.what());
        return false;
    }

    /* GATT discovery happens in connect(); list services (also verifies discovery). */
    for (Service &s : p.services()) {
        printf("    service %s", s.uuid().c_str());
        for (Characteristic &c : s.characteristics()) {
            printf(" [char %s %s%s%s%s%s]", c.uuid().c_str(),
                   c.can_read() ? "R" : "-", c.can_write_command() ? "C" : "-",
                   c.can_write_request() ? "W" : "-", c.can_notify() ? "N" : "-",
                   c.can_indicate() ? "I" : "-");
        }
        printf("\n");
    }

    try {
        p.notify(LOCKINK_SERVICE_UUID, LOCKINK_LISTEN_UUID, on_notify);
    } catch (const std::exception &e) {
        printf("Warning: could not subscribe to notifications (00008ac2-...): %s\n", e.what());
    }
    sleep_ms(PRE_WRITE_DELAY_MS); /* the app's "manufacturer pattern" pre-write delay */
    return true;
}

/* Find the write characteristic (8ac1) and pick a supported write mode.
   Some LockInk devices expose 8ac1 with only write-without-response
   (the app uses writeBytesNoWait for lock/unlock); others also support
   write-with-response. Auto-select so both work. */
static bool write_frame(Peripheral &p, const char *label, const ByteArray &frame) {
    printf("%s: ", label);
    hex_print(frame.data(), frame.size());

    bool wr = false, wc = false;
    for (Service &s : p.services()) {
        for (Characteristic &c : s.characteristics()) {
            if (uuid_ieq(c.uuid(), LOCKINK_WRITE_UUID)) {
                wr = c.can_write_request();
                wc = c.can_write_command();
            }
        }
    }
    if (!wr && !wc) {
        printf("  WRITE FAILED: characteristic %s not found or supports no write mode\n", LOCKINK_WRITE_UUID);
        return false;
    }
    try {
        if (wr) {
            p.write_request(LOCKINK_SERVICE_UUID, LOCKINK_WRITE_UUID, frame);
            printf("  written (write-with-response)\n");
        } else {
            p.write_command(LOCKINK_SERVICE_UUID, LOCKINK_WRITE_UUID, frame);
            printf("  written (write-without-response)\n");
        }
        return true;
    } catch (const std::exception &e) {
        printf("  WRITE FAILED: %s\n", e.what());
        return false;
    }
}

static void disconnect_device(Peripheral &p) {
    try {
        p.unsubscribe(LOCKINK_SERVICE_UUID, LOCKINK_LISTEN_UUID);
    } catch (const std::exception &) {
    }
    try {
        p.disconnect();
    } catch (const std::exception &) {
    }
    sleep_ms(200);
}

/* ------------------------------ commands ------------------------------ */

/* Model = advertised name; fall back to the selector if the backend didn't report a name. */
static const std::string &effective_model(const std::string &name, const std::string &selector) {
    static std::string fallback;
    if (is_lockink_name(name)) return name;
    if (is_lockink_name(selector)) return selector;
    fallback = name;
    return fallback;
}

static int cmd_lock(Peripheral &p, const std::string &name) {
    if (str_ieq(name.c_str(), "AA-A1002")) {
        printf("AA-A1002 does not support lock (ignored by the app as well).\n");
        return 1;
    }
    if (!write_frame(p, "LOCK", frame_lock())) return 0;
    sleep_ms(500);
    return 1;
}

static int cmd_unlock(Peripheral &p, const std::string &name) {
    ByteArray frame = str_ieq(name.c_str(), "AA-A1002") ? frame_a1002_unlock() : frame_unlock();
    if (!write_frame(p, "UNLOCK", frame)) return 0;
    sleep_ms(500);
    return 1;
}

/* freq: max 10 (1012), 3 (1001), 1 (1003); voltage 0..100 */
static int cmd_shock(Peripheral &p, const std::string &name, int freq, int voltage, int duration_s) {
    int max_freq = str_ieq(name.c_str(), "AA-A1012") ? 10 : (str_ieq(name.c_str(), "AA-A1001") ? 3 : 1);
    if (voltage < 0) voltage = 0;
    if (voltage > 100) voltage = 100;
    if (freq < 1) freq = 1;
    if (freq > max_freq) freq = max_freq;
    if (duration_s < 1) duration_s = 1;
    if (duration_s > 600) duration_s = 600;

    printf("shock: freq=%d voltage=%d duration=%ds (model max freq %d)\n", freq, voltage, duration_s, max_freq);

    ByteArray frame = classic_frame(freq, voltage);
    if (!write_frame(p, "SHOCK START", frame)) return 0;

    int elapsed_ms = 0;
    int next_keepalive = KEEPALIVE_MS;
    while (elapsed_ms < duration_s * 1000) {
        sleep_ms(100);
        elapsed_ms += 100;
        if (elapsed_ms >= next_keepalive) {
            if (!write_frame(p, "KEEPALIVE (tiny shock ef=4 v=1)", keepalive_frame())) break;
            sleep_ms(1000); /* app pauses ~1 s after each pulse */
            if (!write_frame(p, "RESTORE SHOCK", frame)) break;
            next_keepalive = elapsed_ms + KEEPALIVE_MS;
        }
    }

    if (!write_frame(p, "STOP", classic_frame_stop())) return 0;
    sleep_ms(500);
    return 1;
}

static int cmd_classic(Peripheral &p, int shake_freq, int shake_intensity, int electric_freq, int voltage,
                       int repeat_ms, int duration_s) {
    if (repeat_ms < 100) repeat_ms = 100;
    if (repeat_ms > 1000) repeat_ms = 1000;
    if (duration_s > 600) duration_s = 600;

    ByteArray frame = classic_frame_full(shake_freq, shake_intensity, electric_freq, voltage);
    if (!write_frame(p, "CLASSIC START", frame)) return 0;

    int elapsed_ms = 0;
    while (duration_s <= 0 || elapsed_ms < duration_s * 1000) {
        sleep_ms(repeat_ms);
        elapsed_ms += repeat_ms;
        if (!write_frame(p, "CLASSIC UPDATE", frame)) break;
    }

    write_frame(p, "CLASSIC STOP", classic_frame_stop());
    sleep_ms(500);
    return 1;
}

static int cmd_random(Peripheral &p, const std::string &name, int min_v, int max_v) {
    if (!str_ieq(name.c_str(), "AA-A1012")) {
        printf("Random mode is only supported on AA-A1012 (ignored by the app otherwise).\n");
        return 1;
    }
    if (min_v < 1) min_v = 1;
    if (max_v > 100) max_v = 100;
    if (!write_frame(p, "RANDOM START", random_frame(1, min_v, max_v))) return 0;
    sleep_ms(500);
    if (!write_frame(p, "RANDOM STOP", random_frame(0, min_v, max_v))) return 0;
    sleep_ms(500);
    return 1;
}

static int cmd_berserk(Peripheral &p, const std::string &name, int start) {
    if (!str_ieq(name.c_str(), "AA-A1012")) {
        printf("Berserker mode is only supported on AA-A1012 (ignored by the app otherwise).\n");
        return 1;
    }
    if (!write_frame(p, start ? "BERSERK START" : "BERSERK STOP", berserk_frame(start))) return 0;
    sleep_ms(500);
    return 1;
}

static int cmd_stop(Peripheral &p, int extra_legacy) {
    if (!write_frame(p, "STOP (classic 0,0)", classic_frame_stop())) return 0;
    sleep_ms(500);
    if (extra_legacy) {
        if (!write_frame(p, "STOP (legacy electric)", legacy_electric_stop())) return 0;
        sleep_ms(500);
    }
    return 1;
}

static int cmd_battery(Peripheral &p, const std::string &name) {
    /* Battery query (from NativeShockBluetoothSupport.queryLockinkBattery): the app
       sends it ~2.5 s after connect and waits up to 7 s for the notification. */
    ByteArray frame = str_ieq(name.c_str(), "AA-A1002") ? battery_query_frame_a1002() : battery_query_frame();
    if (!write_frame(p, "BATTERY QUERY", frame)) return 0;
    printf("Waiting for battery notification (data[2]==0x05, data[3]=percent) up to 7 s...\n");
    sleep_ms(7000);
    return 1;
}

/* Connect to every scanned device and check its GATT for the LockInk service. */
static int cmd_probe() {
    g_adapter.scan_start();
    for (int i = 0; i < 80; i++) sleep_ms(100);
    g_adapter.scan_stop();
    auto results = g_adapter.scan_get_results();
    printf("Probing %zu device(s) for LockInk GATT service %s\n", results.size(), LOCKINK_SERVICE_UUID);
    for (auto &p : results) {
        printf("- %s [%s]", p.identifier().c_str(), p.address().c_str());
        if (!connect_device(p)) {
            printf(" (connect failed)\n");
            continue;
        }
        for (Service &s : p.services()) {
            if (uuid_ieq(s.uuid(), LOCKINK_SERVICE_UUID)) {
                printf("    *** LOCKINK SERVICE FOUND on %s ***\n", p.identifier().c_str());
            }
        }
        disconnect_device(p);
    }
    return 0;
}

/* ------------------------------ main ------------------------------ */

static void usage() {
    printf(
        "Usage: lockink-cli <command> <device> [args]\n"
        "  <device> = advertised name (e.g. AA-A1012) or MAC/persistent identifier\n"
        "\n"
        "  scan [timeout_ms=10000]            list all scanned BLE devices\n"
        "  find [timeout_ms=10000]            wait for a LockInk (AA-A10xx) device\n"
        "  lock <dev>                         send lock frame\n"
        "  unlock <dev>                       send unlock frame (AA-A1002: short frame)\n"
        "  shock <dev> <freq> <voltage> <secs>  manual classic shock + keep-alive + auto-stop\n"
        "  classic <dev> <shakeFreq> <shakeIntensity> <efreq> <volt> <repeat_ms> [duration_s]\n"
        "  random <dev> <minV> <maxV>         random mode (AA-A1012 only)\n"
        "  berserk <dev> <1|0>                berserk mode (AA-A1012 only)\n"
        "  stop <dev> [1]                     classic stop frame (+ optional legacy electric stop)\n"
        "  battery <dev>                      connect and wait for battery notification\n"
        "  probe                              connect to every scanned device, dump GATT,\n"
        "                                   flag the LockInk service %s\n",
        LOCKINK_SERVICE_UUID);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    const char *cmd = argv[1];

    auto adapters = Adapter::get_adapters();
    if (adapters.empty()) {
        printf("No BLE adapter found.\n");
        return 1;
    }
    g_adapter = adapters[0];

    if (strcmp(cmd, "probe") == 0) {
        return cmd_probe();
    }

    if (strcmp(cmd, "scan") == 0 || strcmp(cmd, "find") == 0) {
        int timeout_ms = (argc > 2) ? atoi(argv[2]) : 10000;
        g_adapter.scan_start();
        for (int i = 0; i < timeout_ms; i += 100) sleep_ms(100);
        g_adapter.scan_stop();
        auto results = g_adapter.scan_get_results();
        printf("Scanned %zu device(s):\n", results.size());
        for (size_t i = 0; i < results.size(); i++) {
            printf("[%zu] %-8s %s\n", i, results[i].identifier().c_str(), results[i].address().c_str());
        }
        if (strcmp(cmd, "find") == 0) {
            bool found = false;
            for (auto &p : results) {
                if (is_lockink_name(p.identifier())) {
                    printf("Found LockInk: %s [%s]\n", p.identifier().c_str(), p.address().c_str());
                    found = true;
                    break;
                }
            }
            if (!found) {
                printf("No LockInk (AA-A10xx) device found.\n");
                return 1;
            }
        }
        return 0;
    }

    const char *selector = (argc > 2) ? argv[2] : nullptr;
    if (selector == nullptr) {
        usage();
        return 1;
    }

    if (!scan_find(selector, 10000)) {
        printf("Device '%s' not found during scan.\n", selector);
        return 1;
    }
    Peripheral &dev = g_results[0];
    printf("Found: %s [%s]\n", dev.identifier().c_str(), dev.address().c_str());

    if (!connect_device(dev)) {
        return 1;
    }
    const std::string name = effective_model(dev.identifier(), selector);
    int rc = 1;

    if (strcmp(cmd, "lock") == 0) {
        rc = cmd_lock(dev, name);
    } else if (strcmp(cmd, "unlock") == 0) {
        rc = cmd_unlock(dev, name);
    } else if (strcmp(cmd, "shock") == 0) {
        if (argc < 6) {
            printf("shock needs: <mac> <freq> <voltage> <secs>\n");
            return 1;
        }
        rc = cmd_shock(dev, name, atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
    } else if (strcmp(cmd, "classic") == 0) {
        if (argc < 8) {
            printf("classic needs: <mac> <shakeFreq> <shakeIntensity> <efreq> <volt> <repeat_ms> [duration_s]\n");
            return 1;
        }
        rc = cmd_classic(dev, atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]),
                         (argc > 8) ? atoi(argv[8]) : 0);
    } else if (strcmp(cmd, "random") == 0) {
        if (argc < 5) {
            printf("random needs: <mac> <minV> <maxV>\n");
            return 1;
        }
        rc = cmd_random(dev, name, atoi(argv[3]), atoi(argv[4]));
    } else if (strcmp(cmd, "berserk") == 0) {
        if (argc < 4) {
            printf("berserk needs: <mac> <1|0>\n");
            return 1;
        }
        rc = cmd_berserk(dev, name, atoi(argv[3]));
    } else if (strcmp(cmd, "stop") == 0) {
        rc = cmd_stop(dev, (argc > 3) && atoi(argv[3]));
    } else if (strcmp(cmd, "battery") == 0) {
        rc = cmd_battery(dev, name);
    } else {
        usage();
        return 1;
    }

    disconnect_device(dev);
    return rc ? 0 : 1; /* cmd_* returns 1 on success */
}

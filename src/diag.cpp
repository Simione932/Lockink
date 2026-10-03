// lockink-diag — minimal SimpleBLE C++ diagnostic.
// Connects to the first AA-A10xx device found, lists GATT, subscribes to the
// LockInk listen characteristic, and prints notifications (battery decode included).
// Useful for isolating GATT/subscription issues from the main CLI.
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

#include <simpleble/SimpleBLE.h>

int main() {
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        std::cout << "no adapters\n";
        return 1;
    }
    SimpleBLE::Adapter adapter = adapters[0];

    SimpleBLE::Peripheral target;
    bool have = false;
    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral p) {
        std::cout << "scan found: " << p.identifier() << " [" << p.address() << "]\n";
        if (p.identifier().find("AA-A10") == std::string::npos) return;
        target = p;
        have = true;
    });
    adapter.scan_start();
    for (int i = 0; i < 60 && !have; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    adapter.scan_stop();

    if (!have) {
        std::cout << "no AA-A10xx found\n";
        return 1;
    }

    try {
        target.connect();
        std::cout << "connected\n";
        auto svcs = target.services();
        for (auto &s : svcs) {
            std::cout << "  svc " << s.uuid() << "\n";
            auto chars = s.characteristics();
            for (auto &c : chars) {
                std::cout << "    char " << c.uuid()
                          << " notify=" << c.can_notify()
                          << " write=" << c.can_write_request()
                          << " write_cmd=" << c.can_write_command() << "\n";
            }
        }
        try {
            target.notify("00008ac0-0000-1000-8000-00805f9b34fb",
                         "00008ac2-0000-1000-8000-00805f9b34fb",
                         [](SimpleBLE::ByteArray data) {
                             std::cout << "NOTIFY:";
                             for (auto b : data) std::cout << " " << std::hex << (int)b;
                             std::cout << "\n";
                             if (data.size() >= 4 && data[2] == 0x05 && data[3] <= 100) {
                                 std::cout << "  battery: " << (int)data[3] << "%\n";
                             }
                         });
            std::cout << "subscribed OK\n";
        } catch (const std::exception &e) {
            std::cout << "SUBSCRIBE FAILED: " << e.what() << "\n";
        }
        std::this_thread::sleep_for(std::chrono::seconds(10));
        target.unsubscribe("00008ac0-0000-1000-8000-00805f9b34fb",
                           "00008ac2-0000-1000-8000-00805f9b34fb");
        target.disconnect();
    } catch (const std::exception &e) {
        std::cout << "CONNECT FAILED: " << e.what() << "\n";
    }
    return 0;
}

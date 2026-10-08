// Android 11+ wireless-debugging QR pairing: "WIFI:T:ADB;S:<service>;P:<password>;;"
// The phone, after scanning, advertises _adb-tls-pairing._tcp with instance
// name <service> on the pairing port; `adb pair ip:port <password>` then works.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pm::pairing {

struct Credentials {
    std::string service;   // e.g. "zizai-Ab3dE6fGh9"
    std::string password;  // 12 random [A-Za-z0-9]
    std::string qrText() const { return "WIFI:T:ADB;S:" + service + ";P:" + password + ";;"; }
};
Credentials randomCredentials();  // BCryptGenRandom

// QR (ECC medium, byte mode) → square BGRA image: `scale` px per module,
// `border` modules of white quiet zone. Returns false if the text is too long.
bool renderQr(const std::string& text, int scale, int border, std::vector<uint8_t>& bgra, int& size);
// Module matrix (true = dark), for tests / console output.
bool qrModules(const std::string& text, std::vector<std::vector<bool>>& modules);

}  // namespace pm::pairing

// Minimal mDNS / DNS-SD browser (RFC 6762/6763) for the adb Wi-Fi services
// (_adb-tls-pairing._tcp, _adb-tls-connect._tcp). Independent of adb's own
// mDNS so discovery works even when adb's backend sees nothing (several
// NICs, VPN adapters); AndroidSource uses both.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "adb.h"

namespace pm::mdns {

// ---- DNS message helpers (pure, unit-tested) ----
enum : uint16_t { kTypeA = 1, kTypePtr = 12, kTypeTxt = 16, kTypeAaaa = 28, kTypeSrv = 33, kTypeAny = 255 };

struct Question {
    std::string name;
    uint16_t type = kTypePtr;
    bool unicastResponse = false;  // QU bit
};
std::vector<uint8_t> buildQuery(uint16_t id, const std::vector<Question>& qs);

struct Record {
    std::string name;
    uint16_t type = 0;
    uint32_t ttl = 0;
    // PTR target / SRV target
    std::string target;
    uint16_t port = 0;   // SRV
    uint32_t ipv4 = 0;   // A, host byte order
};
struct Message {
    uint16_t id = 0;
    bool response = false;
    std::vector<Question> questions;
    std::vector<Record> records;  // answers + authority + additional
};
bool parseMessage(const uint8_t* p, size_t n, Message& out);

// Builds a DNS-SD response announcing instance.type.local → ip:port
// (PTR + SRV + A); used by tests and the fake responder.
std::vector<uint8_t> buildServiceResponse(uint16_t id, const std::string& instance, const std::string& type,
                                          const std::string& host, uint32_t ipv4, uint16_t port,
                                          const std::vector<Question>& echoQuestions = {});

// ---- browser ----
class Browser {
public:
    ~Browser();
    // types: e.g. {"_adb-tls-pairing._tcp", "_adb-tls-connect._tcp"}.
    // targets: "ip:port" to query instead of the mDNS group (tests); empty =
    // 224.0.0.251:5353 on every IPv4 interface (+ listening on :5353).
    bool start(std::vector<std::string> types, std::vector<std::string> targets = {});
    void stop();
    std::vector<adb::MdnsService> services() const;
    std::function<void(const adb::MdnsService&)> onFound;  // browser thread
    std::function<void(const std::string&)> log;
    int intervalMs = 1000;

private:
    void run();
    void handle(const Message& m);
    void sendQueries();
    std::vector<std::string> types_;
    std::vector<std::string> targets_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    uintptr_t uni_ = ~uintptr_t(0), multi_ = ~uintptr_t(0);  // SOCKETs
    mutable std::mutex mu_;
    std::map<std::string, std::set<std::string>> ptr_;            // type.local → instance full names
    std::map<std::string, std::pair<std::string, uint16_t>> srv_;  // instance full → host, port
    std::map<std::string, uint32_t> a_;                            // host → ipv4
    std::map<std::string, adb::MdnsService> resolved_;             // instance full → service
};

}  // namespace pm::mdns

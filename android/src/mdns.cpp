#include "mdns.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace pm::mdns {

namespace {
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, uint16_t(x >> 16)); put16(v, uint16_t(x)); }

// Encodes a dotted name. The first label of a DNS-SD instance may contain
// dots -- callers pass {instance, rest} via putNameParts instead.
void putLabels(std::vector<uint8_t>& v, const std::string& dotted) {
    size_t i = 0;
    while (i < dotted.size()) {
        size_t e = dotted.find('.', i);
        if (e == std::string::npos) e = dotted.size();
        size_t len = std::min<size_t>(e - i, 63);
        if (len) {
            v.push_back(uint8_t(len));
            v.insert(v.end(), dotted.begin() + i, dotted.begin() + i + len);
        }
        i = e + 1;
    }
}
void putName(std::vector<uint8_t>& v, const std::string& dotted) {
    putLabels(v, dotted);
    v.push_back(0);
}
void putInstanceName(std::vector<uint8_t>& v, const std::string& instance, const std::string& rest) {
    size_t len = std::min<size_t>(instance.size(), 63);
    v.push_back(uint8_t(len));
    v.insert(v.end(), instance.begin(), instance.begin() + len);
    putName(v, rest);
}

bool readName(const uint8_t* p, size_t n, size_t& off, std::string& out) {
    out.clear();
    size_t pos = off;
    bool jumped = false;
    int hops = 0;
    while (true) {
        if (pos >= n) return false;
        uint8_t len = p[pos];
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= n || ++hops > 32) return false;
            size_t ptr = size_t(((len & 0x3F) << 8) | p[pos + 1]);
            if (!jumped) off = pos + 2;
            jumped = true;
            pos = ptr;
            continue;
        }
        if (len & 0xC0) return false;
        if (len == 0) {
            if (!jumped) off = pos + 1;
            return true;
        }
        if (pos + 1 + len > n) return false;
        if (!out.empty()) out.push_back('.');
        out.append(reinterpret_cast<const char*>(p + pos + 1), len);
        pos += 1 + len;
    }
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(tolower(uint8_t(c)));
    return s;
}

std::string ipStr(uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) +
           "." + std::to_string(ip & 255);
}

struct WsaInit {
    WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WsaInit() { WSACleanup(); }
};

std::vector<uint32_t> ipv4Interfaces() {  // network byte order
    std::vector<uint32_t> v;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    }
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size) != NO_ERROR) return v;
    for (auto a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->Flags & IP_ADAPTER_NO_MULTICAST) continue;
        for (auto u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            v.push_back(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
        }
    }
    return v;
}
}  // namespace

std::vector<uint8_t> buildQuery(uint16_t id, const std::vector<Question>& qs) {
    std::vector<uint8_t> v;
    put16(v, id);
    put16(v, 0);
    put16(v, uint16_t(qs.size()));
    put16(v, 0); put16(v, 0); put16(v, 0);
    for (auto& q : qs) {
        putName(v, q.name);
        put16(v, q.type);
        put16(v, uint16_t(0x0001 | (q.unicastResponse ? 0x8000 : 0)));
    }
    return v;
}

bool parseMessage(const uint8_t* p, size_t n, Message& m) {
    if (n < 12) return false;
    m = {};
    m.id = uint16_t((p[0] << 8) | p[1]);
    m.response = (p[2] & 0x80) != 0;
    uint16_t qd = uint16_t((p[4] << 8) | p[5]);
    uint32_t rr = uint32_t((p[6] << 8) | p[7]) + uint32_t((p[8] << 8) | p[9]) + uint32_t((p[10] << 8) | p[11]);
    size_t off = 12;
    for (uint16_t i = 0; i < qd; ++i) {
        Question q;
        if (!readName(p, n, off, q.name) || off + 4 > n) return false;
        q.type = uint16_t((p[off] << 8) | p[off + 1]);
        q.unicastResponse = (p[off + 2] & 0x80) != 0;
        off += 4;
        m.questions.push_back(std::move(q));
    }
    for (uint32_t i = 0; i < rr; ++i) {
        Record r;
        if (!readName(p, n, off, r.name) || off + 10 > n) return false;
        r.type = uint16_t((p[off] << 8) | p[off + 1]);
        r.ttl = (uint32_t(p[off + 4]) << 24) | (uint32_t(p[off + 5]) << 16) | (uint32_t(p[off + 6]) << 8) | p[off + 7];
        uint16_t rdlen = uint16_t((p[off + 8] << 8) | p[off + 9]);
        off += 10;
        if (off + rdlen > n) return false;
        size_t rd = off;
        if (r.type == kTypePtr) {
            if (!readName(p, n, rd, r.target)) return false;
        } else if (r.type == kTypeSrv && rdlen >= 7) {
            r.port = uint16_t((p[off + 4] << 8) | p[off + 5]);
            rd = off + 6;
            if (!readName(p, n, rd, r.target)) return false;
        } else if (r.type == kTypeA && rdlen == 4) {
            r.ipv4 = (uint32_t(p[off]) << 24) | (uint32_t(p[off + 1]) << 16) | (uint32_t(p[off + 2]) << 8) | p[off + 3];
        }
        off += rdlen;
        m.records.push_back(std::move(r));
    }
    return true;
}

std::vector<uint8_t> buildServiceResponse(uint16_t id, const std::string& instance, const std::string& type,
                                          const std::string& host, uint32_t ipv4, uint16_t port,
                                          const std::vector<Question>& echo) {
    std::vector<uint8_t> v;
    put16(v, id);
    put16(v, 0x8400);  // response, authoritative
    put16(v, uint16_t(echo.size()));
    put16(v, 1);  // answer: PTR
    put16(v, 0);
    put16(v, 2);  // additional: SRV, A
    for (auto& q : echo) {
        putName(v, q.name);
        put16(v, q.type);
        put16(v, 1);
    }
    const std::string typeLocal = type + ".local";
    // PTR
    putName(v, typeLocal);
    put16(v, kTypePtr); put16(v, 1); put32(v, 120);
    std::vector<uint8_t> rd;
    putInstanceName(rd, instance, typeLocal);
    put16(v, uint16_t(rd.size()));
    v.insert(v.end(), rd.begin(), rd.end());
    // SRV
    putInstanceName(v, instance, typeLocal);
    put16(v, kTypeSrv); put16(v, 0x8001); put32(v, 120);
    rd.clear();
    put16(rd, 0); put16(rd, 0); put16(rd, port);
    putName(rd, host);
    put16(v, uint16_t(rd.size()));
    v.insert(v.end(), rd.begin(), rd.end());
    // A
    putName(v, host);
    put16(v, kTypeA); put16(v, 0x8001); put32(v, 120);
    put16(v, 4);
    put32(v, ipv4);
    return v;
}

// --------------------------------------------------------------- Browser --

Browser::~Browser() { stop(); }

bool Browser::start(std::vector<std::string> types, std::vector<std::string> targets) {
    stop();
    static WsaInit wsa;
    types_ = std::move(types);
    targets_ = std::move(targets);
    stop_ = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ptr_.clear(); srv_.clear(); a_.clear(); resolved_.clear();
    }

    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u == INVALID_SOCKET) return false;
    sockaddr_in any{};
    any.sin_family = AF_INET;
    bind(u, reinterpret_cast<sockaddr*>(&any), sizeof(any));
    int ttl = 255;
    setsockopt(u, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<char*>(&ttl), sizeof(ttl));
    uni_ = u;

    if (targets_.empty()) {
        // Shared :5353 listener for multicast replies / announcements.
        SOCKET m = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BOOL yes = TRUE;
        setsockopt(m, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes), sizeof(yes));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(5353);
        if (m != INVALID_SOCKET && bind(m, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
            int joined = 0;
            for (uint32_t ifa : ipv4Interfaces()) {
                ip_mreq mr{};
                inet_pton(AF_INET, "224.0.0.251", &mr.imr_multiaddr);
                mr.imr_interface.s_addr = ifa;
                if (setsockopt(m, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<char*>(&mr), sizeof(mr)) == 0) ++joined;
            }
            setsockopt(m, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<char*>(&ttl), sizeof(ttl));
            multi_ = m;
            if (log) log("mdns: listening on :5353 (" + std::to_string(joined) + " interfaces)");
        } else {
            if (m != INVALID_SOCKET) closesocket(m);
            if (log) log("mdns: :5353 busy, legacy-unicast queries only");
        }
    }
    thread_ = std::thread([this] { run(); });
    return true;
}

void Browser::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (uni_ != ~uintptr_t(0)) closesocket(SOCKET(uni_));
    if (multi_ != ~uintptr_t(0)) closesocket(SOCKET(multi_));
    uni_ = multi_ = ~uintptr_t(0);
}

std::vector<adb::MdnsService> Browser::services() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<adb::MdnsService> v;
    for (auto& [k, s] : resolved_) v.push_back(s);
    return v;
}

void Browser::sendQueries() {
    std::vector<Question> qs;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& t : types_) qs.push_back({t + ".local", kTypePtr, false});
        // Instances seen without SRV, hosts without A.
        for (auto& [type, insts] : ptr_)
            for (auto& inst : insts)
                if (!srv_.count(lower(inst))) qs.push_back({inst, kTypeSrv, false});
        for (auto& [inst, hp] : srv_)
            if (!a_.count(lower(hp.first))) qs.push_back({hp.first, kTypeA, false});
    }
    auto q = buildQuery(0, qs);
    auto legacy = buildQuery(uint16_t(GetTickCount64() | 1), qs);

    if (!targets_.empty()) {
        for (auto& t : targets_) {
            std::string host;
            uint16_t port;
            if (!adb::splitHostPort(t, host, port)) continue;
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(port);
            inet_pton(AF_INET, host.c_str(), &to.sin_addr);
            sendto(SOCKET(uni_), reinterpret_cast<const char*>(legacy.data()), int(legacy.size()), 0,
                   reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }
        return;
    }
    sockaddr_in grp{};
    grp.sin_family = AF_INET;
    grp.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &grp.sin_addr);
    for (uint32_t ifa : ipv4Interfaces()) {
        in_addr ia{};
        ia.s_addr = ifa;
        // Standard query from :5353 (multicast answers) + legacy unicast one.
        if (multi_ != ~uintptr_t(0)) {
            setsockopt(SOCKET(multi_), IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<char*>(&ia), sizeof(ia));
            sendto(SOCKET(multi_), reinterpret_cast<const char*>(q.data()), int(q.size()), 0,
                   reinterpret_cast<sockaddr*>(&grp), sizeof(grp));
        }
        setsockopt(SOCKET(uni_), IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<char*>(&ia), sizeof(ia));
        sendto(SOCKET(uni_), reinterpret_cast<const char*>(legacy.data()), int(legacy.size()), 0,
               reinterpret_cast<sockaddr*>(&grp), sizeof(grp));
    }
}

void Browser::handle(const Message& m) {
    if (!m.response) return;
    std::vector<adb::MdnsService> found;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& r : m.records) {
            if (r.type == kTypePtr) {
                for (auto& t : types_)
                    if (lower(r.name) == lower(t + ".local")) ptr_[lower(r.name)].insert(r.target);
            } else if (r.type == kTypeSrv) {
                srv_[lower(r.name)] = {r.target, r.port};
            } else if (r.type == kTypeA) {
                a_[lower(r.name)] = r.ipv4;
            }
        }
        for (auto& [typeLocal, insts] : ptr_) {
            for (auto& inst : insts) {
                auto s = srv_.find(lower(inst));
                if (s == srv_.end()) continue;
                auto a = a_.find(lower(s->second.first));
                if (a == a_.end()) continue;
                adb::MdnsService svc;
                svc.type = typeLocal.substr(0, typeLocal.size() - 6);  // strip ".local"
                size_t cut = inst.size() >= typeLocal.size() + 1 ? inst.size() - typeLocal.size() - 1 : 0;
                svc.instance = inst.substr(0, cut);
                svc.host = ipStr(a->second);
                svc.port = s->second.second;
                auto& slot = resolved_[lower(inst)];
                if (slot.host != svc.host || slot.port != svc.port) {
                    slot = svc;
                    found.push_back(svc);
                }
            }
        }
    }
    for (auto& s : found) {
        if (log) log("mdns: " + s.instance + " " + s.type + " " + s.hostPort());
        if (onFound) onFound(s);
    }
}

void Browser::run() {
    auto next = std::chrono::steady_clock::now();
    int round = 0;
    while (!stop_) {
        auto now = std::chrono::steady_clock::now();
        if (now >= next) {
            sendQueries();
            // 3 quick rounds, then the steady interval.
            next = now + std::chrono::milliseconds(round++ < 3 ? 250 : intervalMs);
        }
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(SOCKET(uni_), &rs);
        if (multi_ != ~uintptr_t(0)) FD_SET(SOCKET(multi_), &rs);
        timeval tv{0, 50000};
        if (select(0, &rs, nullptr, nullptr, &tv) <= 0) continue;
        for (uintptr_t s : {uni_, multi_}) {
            if (s == ~uintptr_t(0) || !FD_ISSET(SOCKET(s), &rs)) continue;
            uint8_t buf[9000];
            int n = recvfrom(SOCKET(s), reinterpret_cast<char*>(buf), sizeof(buf), 0, nullptr, nullptr);
            if (n <= 0) continue;
            Message m;
            if (parseMessage(buf, size_t(n), m)) handle(m);
        }
    }
}

}  // namespace pm::mdns

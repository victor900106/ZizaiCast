// LAN interfaces for the share server and its QR code: the same choice as the
// AirPlay mDNS responder (core/src/mdnsd/mdnsd.c mdnsd_scan_interfaces), so
// the page is reachable wherever phones can see the receiver.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cwchar>
#include <vector>

#include "pm/share_server.h"
#include "qrcodegen.hpp"

namespace pm::share {
namespace {

bool containsCi(const wchar_t* hay, const wchar_t* needle) {
    if (!hay || !needle) return false;
    const size_t n = wcslen(needle);
    for (const wchar_t* p = hay; *p; ++p)
        if (_wcsnicmp(p, needle, n) == 0) return true;
    return false;
}

bool adapterIsVirtual(const IP_ADAPTER_ADDRESSES* a) {
    // Mobile Hotspot is a real radio link that phones join: keep it.
    static const wchar_t* real[] = {L"Wi-Fi Direct", L"Hosted Network"};
    static const wchar_t* virt[] = {L"Hyper-V", L"vEthernet", L"WSL",     L"Virtual",   L"VirtualBox", L"VMware",
                                    L"VMnet",   L"Tailscale", L"WireGuard", L"Wintun", L"Surfshark", L"TAP-",
                                    L"TAP ",    L"OpenVPN",   L"ZeroTier", L"NordLynx", L"ProtonVPN", L"Npcap",
                                    L"Loopback", L"Bluetooth", L"Docker", L"Kernel Debug", L"Teredo", L"isatap"};
    for (const wchar_t* r : real)
        if (containsCi(a->Description, r) || containsCi(a->FriendlyName, r)) return false;
    if (a->IfType == IF_TYPE_PROP_VIRTUAL || a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PPP) return true;
    for (const wchar_t* v : virt)
        if (containsCi(a->Description, v) || containsCi(a->FriendlyName, v)) return true;
    return false;
}

std::string utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? size_t(n - 1) : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

std::vector<LanInterface> lanInterfaces() {
    struct Cand {
        LanInterface f;
        unsigned long metric;
    };
    std::vector<Cand> all;
    ULONG len = 16 * 1024;
    std::vector<uint8_t> buf;
    DWORD rc = ERROR_BUFFER_OVERFLOW;
    for (int i = 0; i < 3 && rc == ERROR_BUFFER_OVERFLOW; ++i) {
        buf.resize(len);
        rc = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &len);
    }
    if (rc != NO_ERROR) return {};
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
            !(a->Flags & IP_ADAPTER_IPV4_ENABLED))
            continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            if (!sin || sin->sin_family != AF_INET) continue;
            const uint32_t host = ntohl(sin->sin_addr.s_addr);
            if ((host >> 24) == 127 || (host >> 16) == 0xA9FE || host == 0 || u->DadState != IpDadStatePreferred)
                continue;
            Cand c;
            c.f.addr = sin->sin_addr.s_addr;
            ULONG mask = 0;
            ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &mask);
            c.f.mask = mask;
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            c.f.ip = ip;
            c.f.name = utf8(a->FriendlyName);
            c.f.gateway = a->FirstGatewayAddress != nullptr;
            c.f.isVirtual = adapterIsVirtual(a);
            c.metric = a->Ipv4Metric;
            all.push_back(std::move(c));
            break;  // one address per adapter
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const Cand& x, const Cand& y) {
        if (x.f.isVirtual != y.f.isVirtual) return !x.f.isVirtual;
        if (x.f.gateway != y.f.gateway) return x.f.gateway;
        return x.metric < y.metric;
    });
    const bool anyReal = std::any_of(all.begin(), all.end(), [](const Cand& c) { return !c.f.isVirtual; });
    std::vector<LanInterface> out;
    for (auto& c : all) {
        // VPN / VM adapters only when no real one is up, and then only with a gateway.
        if (c.f.isVirtual && (anyReal || !c.f.gateway)) continue;
        out.push_back(c.f);
    }
    return out;
}

bool renderQr(const std::string& text, int scale, int border, std::vector<uint8_t>& bgra, int& size) {
    if (scale < 1 || border < 0) return false;
    try {
        const auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int n = qr.getSize();
        size = (n + 2 * border) * scale;
        bgra.assign(size_t(size) * size_t(size) * 4, 0xFF);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                if (!qr.getModule(x, y)) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    uint8_t* row = &bgra[(size_t((y + border) * scale + dy) * size_t(size) + size_t((x + border) * scale)) * 4];
                    for (int dx = 0; dx < scale; ++dx) row[dx * 4] = row[dx * 4 + 1] = row[dx * 4 + 2] = 0;
                }
            }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace pm::share

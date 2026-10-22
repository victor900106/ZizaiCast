// 自在投影 app: 0.8.3.6 記住這台裝置 -- the PC's list of remembered phones / browsers (remote_devices.txt),
// pure: no file, no lock, no UI (remote_actions.cpp reads / writes the file around it under RemoteState::dmu;
// tested by app/tools/pm_auth_policy_test.cpp).
//
// One line per device and way in: "<device id>\t<label>\t<date>\t<scope>".
// - device id: the phone page's own random id (made by the page from crypto random numbers, kept in that
//   browser's storage). Never an IP address or anything else about the phone; a new browser = a new id.
// - label: "iPhone · Safari" (from the user agent), for the list on the PC only.
// - scope (pm::remote::approvalScope): "lan" (LAN / Tailscale link), "fixed" (主帳號 on the fixed address),
//   "guest:<account id>" (that 臨時帳號 only), "temp" (the temporary address). Lines from before 0.8.3.6
//   have none: "lan" (they were written for the LAN / Tailscale link, or the fixed address with
//   外面的新手機也要允許: the owner's own ways in, one list).
// "lan" and "fixed" are the owner's own ways in and count for each other (as before 0.8.3.6). A 臨時帳號's
// entry counts for that account only, a 臨時網址 entry for the temporary address only.
// Removed: 拒絕 / 中斷 on the PC (every entry of that device), a 臨時帳號 deleted / expired / used up /
// 重設密碼 (its entries), the 主帳號 password changed ("fixed" entries), one by one or all from the menu.
#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace pm_app {

struct RememberedPhone {
    std::string id, label, date, scope;
};

class RememberedDevices {
public:
    static constexpr size_t kMax = 200;  // oldest first out

    static bool validId(const std::string& id) {
        if (id.size() < 8 || id.size() > 64) return false;
        for (char c : id)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
        return true;
    }
    static bool validScope(const std::string& s) {
        if (s == "lan" || s == "fixed" || s == "temp") return true;
        if (s.rfind("guest:", 0) != 0 || s.size() <= 6 || s.size() > 6 + 64) return false;
        for (size_t i = 6; i < s.size(); ++i) {
            const char c = s[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
        }
        return true;
    }
    static bool ownerScope(const std::string& s) { return s == "lan" || s == "fixed"; }
    // Does an entry of scope `entry` count for a session of scope `asked`?
    static bool scopeMatches(const std::string& entry, const std::string& asked) {
        return entry == asked || (ownerScope(entry) && ownerScope(asked));
    }
    static std::string cleanLabel(const std::string& label) {
        std::string l = label.substr(0, 60);
        for (char& c : l)
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return l;
    }

    void parse(const std::string& text) {
        v_.clear();
        size_t a = 0;
        while (a < text.size()) {
            size_t b = text.find('\n', a);
            if (b == std::string::npos) b = text.size();
            std::string line = text.substr(a, b - a);
            a = b + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            RememberedPhone r;
            std::string* f[4] = {&r.id, &r.label, &r.date, &r.scope};
            size_t p = 0;
            for (int i = 0; i < 4; ++i) {
                const size_t q = line.find('\t', p);
                *f[i] = line.substr(p, q == std::string::npos ? std::string::npos : q - p);
                if (q == std::string::npos) break;
                p = q + 1;
            }
            if (r.scope.empty()) r.scope = "lan";  // before 0.8.3.6
            if (!validId(r.id) || !validScope(r.scope) || has(r.id, r.scope, true)) continue;
            r.label = cleanLabel(r.label);
            v_.push_back(std::move(r));
        }
        while (v_.size() > kMax) v_.erase(v_.begin());
    }
    std::string serialize() const {
        std::string out;
        for (const auto& r : v_) out += r.id + '\t' + r.label + '\t' + r.date + '\t' + r.scope + '\n';
        return out;
    }
    // 允許 with 記住 ticked. false: not added (a bad id / scope, or already there).
    bool add(const std::string& id, const std::string& label, const std::string& date, const std::string& scope) {
        if (!validId(id) || !validScope(scope) || has(id, scope, true)) return false;
        v_.push_back({id, cleanLabel(label), date.substr(0, 16), scope});
        while (v_.size() > kMax) v_.erase(v_.begin());
        return true;
    }
    // Lets this device in for a session of this scope (exact = this very scope only).
    bool has(const std::string& id, const std::string& scope, bool exact = false) const {
        for (const auto& r : v_)
            if (r.id == id && (exact ? r.scope == scope : scopeMatches(r.scope, scope))) return true;
        return false;
    }
    // 拒絕 / 中斷 on the PC: every entry of that device.
    size_t forgetDevice(const std::string& id) {
        return eraseIf([&](const RememberedPhone& r) { return !id.empty() && r.id == id; });
    }
    // The 主帳號 password changed ("fixed"), a 臨時帳號 deleted / 重設密碼 ("guest:<id>").
    size_t forgetScope(const std::string& scope) {
        return eraseIf([&](const RememberedPhone& r) { return r.scope == scope; });
    }
    // 臨時帳號 that are gone (expired / used up / deleted): the entries of an account not in ids.
    size_t keepGuests(const std::set<std::string>& ids) {
        return eraseIf([&](const RememberedPhone& r) { return r.scope.rfind("guest:", 0) == 0 && !ids.count(r.scope.substr(6)); });
    }
    bool removeAt(size_t i, RememberedPhone* gone = nullptr) {
        if (i >= v_.size()) return false;
        if (gone) *gone = v_[i];
        v_.erase(v_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    void clear() { v_.clear(); }
    const std::vector<RememberedPhone>& list() const { return v_; }
    size_t size() const { return v_.size(); }

private:
    template <class F>
    size_t eraseIf(F f) {
        const size_t before = v_.size();
        for (auto it = v_.begin(); it != v_.end();) it = f(*it) ? v_.erase(it) : std::next(it);
        return before - v_.size();
    }
    std::vector<RememberedPhone> v_;
};

}  // namespace pm_app

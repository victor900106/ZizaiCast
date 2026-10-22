#include "custom_keys.h"

#include <nlohmann/json.hpp>

#include "input.h"

namespace pm::remote {

namespace {

using nlohmann::json;

int modRank(const std::string& k) { return k == "Control" ? 0 : k == "Alt" ? 1 : k == "Shift" ? 2 : k == "Meta" ? 3 : -1; }

bool fail(std::string* why, const char* w) {
    if (why) *why = w;
    return false;
}

}  // namespace

std::string defaultCustomKeys() {
    return R"([{"c":["Meta","V"],"n":"","t":"dClip"},{"c":["Control","Shift","T"],"n":"","t":"dReopen"},{"c":["Meta","L"],"n":"","t":"dLock"}])";
}

bool sanitizeCustomKeys(const std::string& text, std::string& out, std::string* why) {
    out.clear();
    if (text.size() > 8192) return fail(why, "size");
    json in = json::parse(text, nullptr, false);
    if (in.is_discarded() || !in.is_array()) return fail(why, "parse");
    if (in.size() > kMaxCustomKeys) return fail(why, "count");
    json res = json::array();
    for (const json& e : in) {
        if (!e.is_object()) return fail(why, "type");
        const json n = e.value("n", json("")), c = e.value("c", json::array());
        if (!n.is_string() || !c.is_array()) return fail(why, "type");
        const std::string name = n.get<std::string>();
        if (name.size() > kMaxCustomKeyName) return fail(why, "name");
        for (unsigned char ch : name)
            if (ch < 0x20 || ch == 0x7f) return fail(why, "name");
        std::string t;
        if (e.contains("t")) {
            if (!e["t"].is_string()) return fail(why, "type");
            t = e["t"].get<std::string>();
            if (t != "dClip" && t != "dReopen" && t != "dLock") return fail(why, "type");
        }
        if (c.empty() || c.size() > 5) return fail(why, "combo");
        bool mods[4] = {false, false, false, false};
        std::string main;
        for (const json& k : c) {
            if (!k.is_string()) return fail(why, "combo");
            std::string key = k.get<std::string>();
            if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z') key[0] = char(key[0] - 'a' + 'A');
            if (!keyNameKnown(key)) return fail(why, "key");
            const int r = modRank(key);
            if (r >= 0) {
                if (mods[r]) return fail(why, "combo");
                mods[r] = true;
            } else {
                if (!main.empty()) return fail(why, "combo");  // one key besides the modifiers
                main = key;
            }
        }
        json canon = json::array();
        static const char* const kMods[4] = {"Control", "Alt", "Shift", "Meta"};
        for (int i = 0; i < 4; ++i)
            if (mods[i]) canon.push_back(kMods[i]);
        if (!main.empty()) canon.push_back(main);
        json item{{"n", name}, {"c", canon}};
        if (!t.empty()) item["t"] = t;
        res.push_back(item);
    }
    out = res.dump(-1, ' ', true);  // ensure_ascii: one settings.ini line, whatever the names
    return true;
}

}  // namespace pm::remote

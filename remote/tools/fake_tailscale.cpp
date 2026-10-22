// 0.8.3.2 tests: a FAKE tailscale CLI (never the real one; nothing is changed anywhere). The tests
// point TailscaleFunnel at it (setCliForTest / the app's --dev PM_REMOTE_FAKECLI); it answers the
// commands the app runs from a scenario in PM_FAKE_TS_MODE:
//   ok            Running; funnel --bg works (8443 kept in the PM_FAKE_TS_STATE file)
//   enable-hang   funnel --bg prints "Funnel is not enabled ... visit https://login.tailscale.com/f/funnel?node=..."
//                 and then waits for ever (as the real CLI does until the page is clicked)
//   https-hang    the same for "Serve is not enabled" (HTTPS certificates)
//   silent-hang   funnel --bg prints nothing and never exits
//   no-attr       funnel --bg: Funnel not available; "funnel" node attribute not set (exit 1)
//   old           funnel --bg: flag provided but not defined: -bg (exit 2)
//   needs-login   status: BackendState NeedsLogin
//   port-taken    funnel status: 8443 already serves http://127.0.0.1:9000 (another program)
// PM_FAKE_TS_OFF_MS=<n> (0.8.3.5): "funnel --https=8443 off" takes n ms before it removes 8443.
// Every run appends its PID to PM_FAKE_TS_PIDS (if set), so a test can check none is left running.
// PM_FAKE_TS_MODEFILE: the scenario is read from that file's first line instead (switched by the test).
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

std::string env(const char* k) {
    char b[1024] = {};
    const DWORD n = GetEnvironmentVariableA(k, b, sizeof(b));
    return n && n < sizeof(b) ? std::string(b, n) : std::string();
}
const char* kName = "example-pc.example.ts.net";  // made up

}  // namespace

int main(int argc, char** argv) {
    std::string args;
    for (int i = 1; i < argc; ++i) args += (i > 1 ? " " : "") + std::string(argv[i]);
    std::string mode = env("PM_FAKE_TS_MODE");
    const std::string state = env("PM_FAKE_TS_STATE"), pids = env("PM_FAKE_TS_PIDS"), modeFile = env("PM_FAKE_TS_MODEFILE");
    if (!modeFile.empty()) {  // the test may switch the scenario while the app runs (e.g. "enabled on the page now")
        std::ifstream mf(modeFile);
        std::string m;
        if (std::getline(mf, m) && !m.empty()) mode = m;
    }
    if (!pids.empty()) std::ofstream(pids, std::ios::app) << GetCurrentProcessId() << " " << args << "\n";
    auto kept = [&] {
        std::ifstream in(state);
        std::string t;
        std::getline(in, t);
        return t;
    };
    if (args == "status --json") {
        std::printf("{\"BackendState\":\"%s\",\"Self\":{\"DNSName\":\"%s.\"}}\n", mode == "needs-login" ? "NeedsLogin" : "Running", kName);
        return 0;
    }
    if (args == "funnel status --json") {
        const std::string t = mode == "port-taken" ? std::string("http://127.0.0.1:9000") : kept();
        if (t.empty()) std::printf("{}\n");
        else
            std::printf("{\"TCP\":{\"8443\":{\"HTTPS\":true}},\"Web\":{\"%s:8443\":{\"Handlers\":{\"/\":{\"Proxy\":\"%s\"}}}},\"AllowFunnel\":{\"%s:8443\":true}}\n",
                        kName, t.c_str(), kName);
        return 0;
    }
    if (args.rfind("funnel --bg --https=8443 ", 0) == 0) {
        std::fflush(stdout);
        if (mode == "enable-hang" || mode == "https-hang") {
            const bool https = mode == "https-hang";
            std::printf("%s is not enabled on your tailnet.\nTo enable, visit:\n\n         https://login.tailscale.com/f/%s?node=nFAKE0832\n\n",
                        https ? "Serve" : "Funnel", https ? "serve" : "funnel");
            std::fflush(stdout);
            Sleep(INFINITE);
        }
        if (mode == "silent-hang") Sleep(INFINITE);
        if (mode == "no-attr") {
            std::printf("Funnel not available; \"funnel\" node attribute not set. See https://tailscale.com/s/no-funnel.\n");
            return 1;
        }
        if (mode == "old") {
            std::printf("flag provided but not defined: -bg\nUSAGE\n  funnel <serve-port> {on|off}\n");
            return 2;
        }
        if (!state.empty()) std::ofstream(state, std::ios::trunc) << args.substr(25) << "\n";
        std::printf("Available on the internet:\n\nhttps://%s:8443/\n|-- proxy %s\n", kName, args.substr(25).c_str());
        return 0;
    }
    if (args == "funnel --https=8443 off") {
        // 0.8.3.5: PM_FAKE_TS_OFF_MS = a slow "off" (the off -> on race: the app's next "on" must wait for it)
        const std::string offMs = env("PM_FAKE_TS_OFF_MS");
        if (!offMs.empty()) Sleep(static_cast<DWORD>(std::atoi(offMs.c_str())));
        if (!state.empty()) std::ofstream(state, std::ios::trunc) << "";
        return 0;
    }
    std::printf("fake tailscale: refused \"%s\"\n", args.c_str());
    return 1;
}

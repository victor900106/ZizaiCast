// Stand-in for adb.exe in pm_android_test (AndroidSource state machine
// without a phone or a real adb server): no sockets at all.  One paired
// phone 10.9.8.7:5555 "Fake Phone" is "connected"; `push` takes 4 s (the
// slow start-up step a takeover can interrupt); anything unknown fails.
#include <windows.h>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-P")) i += 2;  // private server port
    if (i + 1 < argc && !strcmp(argv[i], "-s")) i += 2;  // device serial
    if (i >= argc) return 1;
    const char* cmd = argv[i];
    if (!strcmp(cmd, "start-server") || !strcmp(cmd, "kill-server")) return 0;
    if (!strcmp(cmd, "devices")) {
        std::printf("List of devices attached\n10.9.8.7:5555          device product:fake model:Fake_Phone device:fake\n\n");
        return 0;
    }
    if (!strcmp(cmd, "connect") && i + 1 < argc) {
        std::printf("already connected to %s\n", argv[i + 1]);
        return 0;
    }
    if (!strcmp(cmd, "shell")) {
        std::printf("Fake Phone\n");
        return 0;
    }
    if (!strcmp(cmd, "push")) {
        Sleep(4000);
        std::printf("1 file pushed\n");
        return 0;
    }
    if (!strcmp(cmd, "mdns")) {
        std::printf("List of discovered mdns services\n");
        return 0;
    }
    std::fprintf(stderr, "fake adb: unsupported command %s\n", cmd);
    return 1;
}

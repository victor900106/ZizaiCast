// fake_cloudflared (0.8.4 tests): stands in for cloudflared.exe so that the add-on download / launch path can be tested
// without ever running the real tunnel client. Prints a cloudflared-looking error and exits.
#include <cstdio>
int main(int argc, char** argv) {
    std::fprintf(stderr, "2026-01-01T00:00:00Z ERR fake cloudflared (test stand-in), args:");
    for (int i = 1; i < argc; ++i) std::fprintf(stderr, " %s", argv[i]);
    std::fprintf(stderr, "\n");
    return 3;
}

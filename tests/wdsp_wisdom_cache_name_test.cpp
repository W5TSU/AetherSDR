// WdspChannel's FFTW wisdom cache is keyed by the FFTW build.
//
// FFTW rejects wisdom written by any other FFTW build wholesale, and every
// channel open exports. With one unversioned file, a source build (FFTW 3.3.10)
// and the x86_64 AppImage (FFTW 3.3.8) on the same machine each overwrote the
// other's plans, so each one's next cold open re-measured every PATIENT plan --
// on HackRF, a GUI-thread freeze long enough to read as "cannot connect".
//
// Pins: the key comes from FFTW's own wisdom header (version + planner
// signature, the exact thing FFTW checks before accepting wisdom), read through
// an exported FUNCTION: the data symbol fftw_version is not exported by the
// Windows FFTW DLL, and linking against it broke the Windows build. Different
// versions or signatures give different keys; the file is never the legacy
// shared name, stays in the cache directory, and honours the override. No
// channel is opened, so this measures no plans.
#include "core/dsp/WdspChannel.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {
int g_failures = 0;
void check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}
} // namespace

int main()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "aether-wisdom-name-test";
#ifdef _WIN32
    _putenv_s("AETHER_WDSP_WISDOM_DIR", dir.string().c_str());
#else
    ::setenv("AETHER_WDSP_WISDOM_DIR", dir.string().c_str(), 1);
#endif

    // ---- the key, from the header text alone ----
    const char* h3310 = "(fftw-3.3.10 fftw_wisdom #x458a31c8 #x92381c4c #x4f974889 #xcd46f97e\n)\n";
    const char* h338  = "(fftw-3.3.8 fftw_wisdom #x458a31c8 #x92381c4c #x4f974889 #xcd46f97e\n)\n";
    const char* hSig  = "(fftw-3.3.10 fftw_wisdom #x11111111 #x92381c4c #x4f974889 #xcd46f97e\n)\n";
    const std::string k3310 = WdspChannel::wisdomCacheKeyFromHeader(h3310);
    const std::string k338 = WdspChannel::wisdomCacheKeyFromHeader(h338);
    std::printf("key(3.3.10)=%s key(3.3.8)=%s\n", k3310.c_str(), k338.c_str());
    check(k3310.rfind("3.3.10-", 0) == 0, "the key starts with the FFTW version");
    check(k3310 != k338, "a different FFTW version gives a different key");
    check(k3310 != WdspChannel::wisdomCacheKeyFromHeader(hSig),
          "a different planner signature gives a different key");
    check(k3310 == WdspChannel::wisdomCacheKeyFromHeader(h3310), "the key is stable");
    check(WdspChannel::wisdomCacheKeyFromHeader("garbage") == "unknown",
          "an unrecognised header gives a fixed fallback, not an empty name");

    // ---- the file this process uses ----
    const fs::path file = WdspChannel::wisdomCacheFile();
    const std::string name = file.filename().string();
    std::printf("cache=%s\n", name.c_str());
    check(file.parent_path() == dir, "cache lives in the override directory");
    check(name.rfind("wdsp-fftw-wisdom-", 0) == 0, "name keeps the wdsp-fftw-wisdom- prefix");
    check(name != "wdsp-fftw-wisdom", "name is not the legacy shared file");
    check(name.find("unknown") == std::string::npos,
          "this process's FFTW header was recognised");
    check(name.find('/') == std::string::npos && name.find('\\') == std::string::npos,
          "name contains no path separator");

    std::error_code ec;
    fs::remove_all(dir, ec);
    return g_failures == 0 ? 0 : 1;
}

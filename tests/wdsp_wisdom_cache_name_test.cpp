// WdspChannel's FFTW wisdom cache is keyed by the FFTW build.
//
// FFTW rejects wisdom written by any other FFTW build wholesale, and every
// channel open exports. With one unversioned file, a source build (FFTW 3.3.10)
// and the x86_64 AppImage (FFTW 3.3.8) on the same machine each overwrote the
// other's plans, so each one's next cold open re-measured every PATIENT plan --
// on HackRF, a GUI-thread freeze long enough to read as "cannot connect".
//
// Pins: the file name carries this process's FFTW version, it is never the
// legacy shared name, it stays inside the cache directory, and the override
// directory is honoured. No channel is opened, so this measures no plans.
#include "core/dsp/WdspChannel.h"

#include <fftw3.h>

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

    const fs::path file = WdspChannel::wisdomCacheFile();
    const std::string name = file.filename().string();
    std::string version = fftw_version;   // e.g. "fftw-3.3.10-sse2-avx"
    if (version.rfind("fftw-", 0) == 0)
        version.erase(0, 5);

    std::printf("fftw_version=%s cache=%s\n", fftw_version, name.c_str());
    check(file.parent_path() == dir, "cache lives in the override directory");
    check(name.rfind("wdsp-fftw-wisdom-", 0) == 0, "name keeps the wdsp-fftw-wisdom- prefix");
    check(name != "wdsp-fftw-wisdom", "name is not the legacy shared file");
    check(name == "wdsp-fftw-wisdom-" + version,
          "name is keyed by exactly this FFTW build's version string");
    check(name.find('/') == std::string::npos && name.find('\\') == std::string::npos,
          "name contains no path separator");

    std::error_code ec;
    fs::remove_all(dir, ec);
    return g_failures == 0 ? 0 : 1;
}

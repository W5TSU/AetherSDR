#pragma once

#include <mutex>

namespace AetherSDR::hackrf {

// Serialises libhackrf's process-global lifecycle: hackrf_init, hackrf_exit,
// hackrf_device_list(_free), and the open/close calls that move its device
// count. Take it around each of those, and nothing else.
//
// libhackrf keeps ONE global libusb context and a global open-device count, and
// manages both without any locking: hackrf_init creates the context only if it
// is null, and hackrf_exit calls libusb_exit on it whenever the count is zero.
// HackRfDiscovery runs init/list/exit on a pool thread every scan, while
// HackRfWorker runs init/open and close/exit on the GUI thread, so they
// interleave. Two concurrent hackrf_exit calls both pass the null check and
// libusb_exit the same context: observed as a SIGSEGV inside libusb_exit on the
// discovery thread while the backend was closing its device. A scan's exit can
// equally free the context between a connect's hackrf_init and hackrf_open.
//
// Streaming calls (start/stop RX/TX, tuning, gains) do not take this: they work
// on an open device, and the context cannot be freed while one is open.
inline std::mutex& libraryMutex()
{
    static std::mutex m;
    return m;
}

} // namespace AetherSDR::hackrf

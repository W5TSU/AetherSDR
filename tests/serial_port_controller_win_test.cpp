// #99 — on Windows, SerialPortController's output-pin writes and close() must
// not wait behind the watcher thread's pending WaitCommEvent. With the port
// opened synchronously, Windows ran one request on the handle at a time: the
// first PTT edge froze the controller thread until an input pin happened to
// change, and shutdown's blocking close on that thread waited forever, leaving
// AetherSDR.exe running and holding the port.
//
// Needs a real COM pair wired DTR -> DSR (a null-modem cable, or a virtual
// pair such as HHD Virtual Serial Port Tools or com0com):
//   AETHER_SERIAL_TEST_PORT      the side under test
//   AETHER_SERIAL_TEST_FAR_PORT  the other end, driven and read here
// Skips (77) without them and on every non-Windows platform. Nothing else may
// be attached to the pair: the test raises DTR, the configured PTT output.
//
// Each controller call runs on a worker thread, as on the ExtControllers
// thread in MainWindow, against a 1 s deadline. A call that misses it is the
// bug: the test reports it and exits at once, because a thread stuck in a
// Win32 call cannot be joined.

#include "TestEventLoop.h"
#include "core/SerialPortController.h"

#include <QCoreApplication>
#include <QThread>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    std::fflush(stdout);
    failures += ok ? 0 : 1;
}

#ifdef Q_OS_WIN

constexpr int kDeadlineMs = 1000;

// The far end of the pair: plain synchronous Win32, no watcher of its own.
class FarSide {
public:
    ~FarSide()
    {
        if (m_h != INVALID_HANDLE_VALUE) {
            ::EscapeCommFunction(m_h, CLRDTR);
            ::CloseHandle(m_h);
        }
    }

    bool open(const QString& name)
    {
        const QString path = QStringLiteral("\\\\.\\") + name;
        m_h = ::CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                            GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (m_h == INVALID_HANDLE_VALUE) {
            return false;
        }
        DCB dcb = {};
        dcb.DCBlength = sizeof(DCB);
        ::GetCommState(m_h, &dcb);
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        return ::SetCommState(m_h, &dcb) != 0;
    }

    void setDtr(bool on) { ::EscapeCommFunction(m_h, on ? SETDTR : CLRDTR); }

    bool dsr() const
    {
        DWORD status = 0;
        ::GetCommModemStatus(m_h, &status);
        return (status & MS_DSR_ON) != 0;
    }

private:
    HANDLE m_h = INVALID_HANDLE_VALUE;
};

// Run fn on ctrl's thread; false if it has not returned within the deadline.
template <typename Fn>
bool onWorker(QObject* ctrl, Fn fn)
{
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    QMetaObject::invokeMethod(ctrl, [fn, done] {
        fn();
        done->set_value();
    }, Qt::QueuedConnection);
    return finished.wait_for(std::chrono::milliseconds(kDeadlineMs)) == std::future_status::ready;
}

void requireReturned(bool returned, const char* what)
{
    check(returned, what);
    if (!returned) {
        std::printf("%d failure(s) — a controller call is stuck in Win32; exiting\n", failures);
        std::fflush(stdout);
        std::_Exit(1);
    }
}

#endif

} // namespace

int main(int argc, char** argv)
{
#ifndef Q_OS_WIN
    Q_UNUSED(argc);
    Q_UNUSED(argv);
    std::printf("SKIP: Windows only (the Win32 WaitCommEvent path)\n");
    return 77;
#else
    const QString port = qEnvironmentVariable("AETHER_SERIAL_TEST_PORT");
    const QString farName = qEnvironmentVariable("AETHER_SERIAL_TEST_FAR_PORT");
    if (port.isEmpty() || farName.isEmpty()) {
        std::printf("SKIP: set AETHER_SERIAL_TEST_PORT and AETHER_SERIAL_TEST_FAR_PORT "
                    "to the two ends of a DTR->DSR wired COM pair\n");
        return 77;
    }

    QCoreApplication app(argc, argv);

    FarSide far;
    check(far.open(farName), "the far end of the pair opens");
    if (failures) {
        return 1;
    }

    QThread worker;
    worker.setObjectName(QStringLiteral("ExtControllers"));
    auto* ctrl = new SerialPortController;
    ctrl->setDtrFunction(SerialPortController::PinFunction::PTT);
    ctrl->setDtrPolarity(true);
    ctrl->setDsrFunction(SerialPortController::InputFunction::PttInput);
    ctrl->setDsrPolarity(true);
    ctrl->moveToThread(&worker);
    worker.start();

    bool pttIn = false;
    int pttInEdges = 0;
    QObject::connect(ctrl, &SerialPortController::externalPttChanged, &app,
                     [&](bool active) {
                         pttIn = active;
                         ++pttInEdges;
                     });

    for (int cycle = 1; cycle <= 3; ++cycle) {
        std::printf("-- cycle %d\n", cycle);
        bool opened = false;
        requireReturned(onWorker(ctrl, [&] { opened = ctrl->open(port, 19200); }),
                        "open() returns");
        check(opened, "the port under test opens");
        if (!opened) {
            break;
        }
        QThread::msleep(100);  // the watcher's WaitCommEvent is pending

        // Output: the PTT edge reaches the line and does not freeze the thread.
        requireReturned(onWorker(ctrl, [&] { ctrl->setTransmitting(true); }),
                        "setTransmitting(true) returns while the watcher waits");
        check(AetherTest::waitFor([&] { return far.dsr(); }, kDeadlineMs),
              "PTT raises DTR: the far end sees DSR");
        requireReturned(onWorker(ctrl, [&] { ctrl->setTransmitting(false); }),
                        "setTransmitting(false) returns");
        check(AetherTest::waitFor([&] { return !far.dsr(); }, kDeadlineMs),
              "PTT release drops DTR: the far end loses DSR");

        // Input: the watcher still reports a pin edge.
        const int edgesBefore = pttInEdges;
        QThread::msleep(50);  // past the 20 ms input debounce
        far.setDtr(true);
        check(AetherTest::waitFor([&] { return pttInEdges > edgesBefore && pttIn; }, kDeadlineMs),
              "a DSR rise reaches externalPttChanged(true)");
        QThread::msleep(50);
        far.setDtr(false);
        check(AetherTest::waitFor([&] { return pttInEdges > edgesBefore + 1 && !pttIn; }, kDeadlineMs),
              "a DSR fall reaches externalPttChanged(false)");

        // Close with no input edge on the way, as at shutdown.
        bool closed = false;
        requireReturned(onWorker(ctrl, [&] {
                            ctrl->close();
                            closed = !ctrl->isOpen();
                        }),
                        "close() returns with no pin edge pending");
        check(closed, "the port is closed");
    }

    QMetaObject::invokeMethod(ctrl, [ctrl] { delete ctrl; }, Qt::BlockingQueuedConnection);
    worker.quit();
    worker.wait();

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
#endif
}

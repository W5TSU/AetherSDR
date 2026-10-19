#include "HackRfDiscovery.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtGlobal>
#include <QHash>
#include <QSet>

#include <cstdint>

#ifdef AETHER_BACKEND_HACKRF
#include "core/backends/hackrf/HackRfLibraryLock.h"
#include <hackrf.h>
#endif

namespace AetherSDR {

// ---------------------------------------------------------------------------
// HackRfDiscovery  —  pattern-matches RtlSdrDiscovery for ConnectionPanel (#42)
// ---------------------------------------------------------------------------

HackRfDiscovery::HackRfDiscovery(QObject* parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    connect(m_timer, &QTimer::timeout, this, &HackRfDiscovery::onScanTimer);
    connect(&m_watcher, &QFutureWatcher<QVector<RadioInfo>>::finished,
            this, &HackRfDiscovery::onScanFinished);
}

HackRfDiscovery::~HackRfDiscovery()
{
    stop();
    m_watcher.waitForFinished();
}

bool HackRfDiscovery::isRunning() const noexcept
{
    return m_timer->isActive() || m_scanning;
}

bool HackRfDiscovery::isAvailable()
{
#ifdef AETHER_BACKEND_HACKRF
    return true;
#else
    return false;
#endif
}

void HackRfDiscovery::start(int intervalMs)
{
    if (!isAvailable()) {
        return;
    }
    m_timer->start(intervalMs);
    onScanTimer(); // immediate first scan
}

void HackRfDiscovery::stop()
{
    m_timer->stop();
}

// ---------------------------------------------------------------------------
// Scan logic: enumerate USB devices in a background worker thread via QtConcurrent
// ---------------------------------------------------------------------------

void HackRfDiscovery::onScanTimer()
{
    if (!isAvailable() || m_scanning) {
        return; // Background scan still in progress or backend disabled
    }

    m_scanning = true;
    m_watcher.setFuture(QtConcurrent::run([]() -> QVector<RadioInfo> {
        QVector<RadioInfo> current;

#ifdef AETHER_BACKEND_HACKRF
        // hackrf_init()/hackrf_exit() are NOT safe to run concurrently with
        // HackRfWorker's: libhackrf manages its one global libusb context with
        // no locking, and two racing hackrf_exit() calls both libusb_exit it
        // (a SIGSEGV in libusb_exit on this thread, observed). The whole scan
        // holds the library lock HackRfWorker takes around init/open and
        // close/exit; see HackRfLibraryLock.h. libhackrf's own NOT_LAST_DEVICE
        // check still keeps this exit from closing a context a device is open
        // on, which is why streaming needs no lock.
        const std::lock_guard<std::mutex> lock(hackrf::libraryMutex());
        if (hackrf_init() == HACKRF_SUCCESS) {
            hackrf_device_list_t* list = hackrf_device_list();
            if (list) {
                QSet<QString> usedSerials;
                for (int i = 0; i < list->devicecount; ++i) {
                    RadioInfo info;
                    info.family = QStringLiteral("hackrf");

                    const QString usbSerial = (list->serial_numbers && list->serial_numbers[i])
                        ? QString::fromUtf8(list->serial_numbers[i]).trimmed()
                        : QString();
                    info.serialIdentity.reportedSerial = usbSerial;
                    if (!usbSerial.isEmpty() && !usedSerials.contains(usbSerial)) {
                        info.serial = usbSerial;
                    } else {
                        info.serial = QStringLiteral("hackrf:%1").arg(i);
                        info.serialIdentity.indexLocator = true;
                    }
                    usedSerials.insert(info.serial);

                    // The USB product id cannot tell a HackRF Pro from a One —
                    // both enumerate as USB_BOARD_ID_HACKRF_ONE (0x6089), so
                    // the Pro was listed as "HackRF One". The board id can,
                    // but only with the device open: read it once per serial
                    // and remember it. An open that fails (the device is
                    // already streaming for a backend) falls back to the USB
                    // name and is retried on a later scan. The cache is only
                    // touched under the library lock held above.
                    static QHash<QString, QString> boardNameBySerial;
                    QString model = boardNameBySerial.value(info.serial);
                    if (model.isEmpty()) {
                        hackrf_device* dev = nullptr;
                        if (hackrf_device_list_open(list, i, &dev) == HACKRF_SUCCESS && dev) {
                            // 0xFF by number: the older libhackrf the x86_64
                            // AppImage builds against (Ubuntu 22.04) calls it
                            // BOARD_ID_INVALID, the newer BOARD_ID_UNDETECTED.
                            constexpr std::uint8_t kBoardIdUndetected = 0xFF;
                            std::uint8_t id = kBoardIdUndetected;
                            if (hackrf_board_id_read(dev, &id) == HACKRF_SUCCESS
                                && id != kBoardIdUndetected) {
                                // Board id 5 named by hand: the older libhackrf
                                // the AppImages build against predates the Pro
                                // and would name it "Unknown" (see #67).
                                constexpr std::uint8_t kBoardIdPraline = 5;
                                const char* boardName = id == kBoardIdPraline
                                    ? "HackRF Pro"
                                    : hackrf_board_id_name(static_cast<hackrf_board_id>(id));
                                if (boardName && *boardName) {
                                    model = QString::fromUtf8(boardName);
                                    boardNameBySerial.insert(info.serial, model);
                                }
                            }
                            hackrf_close(dev);
                        }
                    }
                    if (model.isEmpty() && list->usb_board_ids) {
                        const char* boardName = hackrf_usb_board_id_name(list->usb_board_ids[i]);
                        if (boardName && *boardName) {
                            model = QString::fromUtf8(boardName);
                        }
                    }
                    if (model.isEmpty())
                        model = QStringLiteral("HackRF");
                    info.model = model;
                    info.name = model;
                    info.status = QStringLiteral("Available");
                    info.inUse = false;

                    current.append(info);
                }
                hackrf_device_list_free(list);
            }
            hackrf_exit();
        }
#endif
        return current;
    }));
}

void HackRfDiscovery::onScanFinished()
{
    m_scanning = false;
    const QVector<RadioInfo> current = m_watcher.result();

    if (!m_timer->isActive()) {
        return;
    }

    // Track which devices are still present; report lost ones
    QStringList lostSerials;
    for (auto it = m_seen.begin(); it != m_seen.end(); ++it) {
        const QString& serial = it.key();
        bool found = false;
        for (const auto& cur : current) {
            if (cur.serial == serial) {
                found = true;
                it.value().missedScans = 0;
                break;
            }
        }
        if (!found) {
            ++it.value().missedScans;
            if (it.value().missedScans >= kMissedScansBeforeLost) {
                lostSerials.append(serial);
            }
        }
    }

    // Remove lost entries
    for (const auto& serial : lostSerials) {
        emit radioLost(serial);
        m_seen.remove(serial);
    }

    // Report newly discovered or updated devices
    for (const auto& info : current) {
        if (!m_seen.contains(info.serial)) {
            m_seen[info.serial] = Seen{info, 0};
            emit radioDiscovered(info);
        } else {
            if (m_seen[info.serial].info.name != info.name ||
                m_seen[info.serial].info.model != info.model ||
                m_seen[info.serial].info.serialIdentity.reportedSerial
                    != info.serialIdentity.reportedSerial ||
                m_seen[info.serial].info.serialIdentity.indexLocator
                    != info.serialIdentity.indexLocator) {
                m_seen[info.serial].info = info;
                emit radioUpdated(info);
            }
        }
    }
}

}  // namespace AetherSDR

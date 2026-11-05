#include "PureSignalDialog.h"

#include "PureSignalCurveWidget.h"
#include "core/ThemeManager.h"
#include "core/TxKeyingMarker.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {
constexpr int kRefreshMs = 200;
constexpr int kTestTickMs = 20;
constexpr int kTestDefaultPower = 5;
constexpr int kTestMaxPower = 80;

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

// Thetis's and deskHPSDR's bands for the feedback meter, as the TX applet shows them.
QString feedbackAdvice(int level)
{
    if (level <= 0)
        return QObject::tr("none yet");
    if (level > 128 && level <= 181)
        return QObject::tr("%1, good").arg(level);
    if (level > 90 && level <= 200)
        return QObject::tr("%1, usable").arg(level);
    return QObject::tr("%1, out of range").arg(level);
}
}

PureSignalDialog::PureSignalDialog(RadioModel* radio, QWidget* parent)
    : PersistentDialog(QStringLiteral("PureSignal"), QStringLiteral("PureSignalDialogGeometry"),
                       parent)
    , m_radio(radio)
{
    setMinimumSize(720, 420);
    resize(900, 520);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(8);

    auto* readouts = new QGridLayout;
    readouts->setHorizontalSpacing(16);
    const auto addReadout = [&](int row, int column, const QString& name) {
        auto* title = new QLabel(name);
        ThemeManager::instance().applyStyleSheet(title,
            "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
        auto* value = new QLabel(QStringLiteral("-"));
        value->setAccessibleName(name);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        readouts->addWidget(title, row, column * 2);
        readouts->addWidget(value, row, column * 2 + 1);
        return value;
    };
    m_state = addReadout(0, 0, tr("State"));
    m_level = addReadout(1, 0, tr("Feedback level"));
    m_gain = addReadout(0, 1, tr("Receive gain"));
    m_calibrations = addReadout(1, 1, tr("Calibrations"));
    m_peaks = addReadout(0, 2, tr("Feedback / reference"));
    m_lastFailure = addReadout(1, 2, tr("Last result"));
    readouts->setColumnStretch(5, 1);
    root->addLayout(readouts);

    auto* plots = new QHBoxLayout;
    m_amplitude = new PureSignalCurveWidget(tr("Amplitude"), tr("drive"), tr("output"),
                                            PureSignalCurveWidget::YAxis::Level, 1.0);
    m_phase = new PureSignalCurveWidget(tr("Phase"), tr("output"), tr("degrees"),
                                        PureSignalCurveWidget::YAxis::AroundZero, 5.0);
    m_correction = new PureSignalCurveWidget(tr("Correction"), tr("output"), tr("gain"),
                                             PureSignalCurveWidget::YAxis::Fitted, 0.2);
    plots->addWidget(m_amplitude);
    plots->addWidget(m_phase);
    plots->addWidget(m_correction);
    root->addLayout(plots, 1);

    auto* note = new QLabel(
        tr("Dots are what the last calibration measured, against the dashed line a linear "
           "amplifier would give. The orange line is the correction. To see whether an "
           "amplifier is in the loop, hold a curve with it in Standby and compare with it "
           "in Operate: if nothing moves, PureSignal isn't seeing it."));
    note->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(note,
        "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    root->addWidget(note);

    auto* buttons = new QHBoxLayout;
    m_curveNote = new QLabel(tr("No calibration yet"));
    m_hold = new QPushButton(tr("Hold curve"));
    m_hold->setToolTip(tr("Keep this calibration's curve on the plots to compare with later ones"));
    m_hold->setEnabled(false);
    m_clearHeld = new QPushButton(tr("Clear held"));
    m_clearHeld->setEnabled(false);
    buttons->addWidget(m_curveNote, 1);
    buttons->addWidget(m_hold);
    buttons->addWidget(m_clearHeld);
    root->addLayout(buttons);
    connect(m_hold, &QPushButton::clicked, this, &PureSignalDialog::holdCurves);
    connect(m_clearHeld, &QPushButton::clicked, this, &PureSignalDialog::clearHeld);

    auto* test = new QHBoxLayout;
    m_testMode = new QComboBox;
    m_testMode->setAccessibleName(tr("Test signal"));
    m_testMode->addItem(tr("Calibrate"), static_cast<int>(PureSignalTestRun::Mode::Calibrate));
    m_testMode->addItem(tr("A/B compare"), static_cast<int>(PureSignalTestRun::Mode::AB));
    m_testMode->addItem(tr("Two-tone"), static_cast<int>(PureSignalTestRun::Mode::TwoTone));
    m_testMode->addItem(tr("Single tone"), static_cast<int>(PureSignalTestRun::Mode::SingleTone));
    m_testMode->setItemData(0, tr("Two-tone until PS has settled, or the time limit"),
                            Qt::ToolTipRole);
    m_testMode->setItemData(1, tr("Two-tone, with the correction off and on every 3 seconds"),
                            Qt::ToolTipRole);
    m_testMode->setItemData(3, tr("A single carrier. PS can't calibrate on this"),
                            Qt::ToolTipRole);
    m_testPower = new QSpinBox;
    m_testPower->setAccessibleName(tr("Test signal power"));
    m_testPower->setRange(1, kTestMaxPower);
    m_testPower->setValue(kTestDefaultPower);
    m_testPower->setSuffix(QStringLiteral(" %"));
    m_testSeconds = new QSpinBox;
    m_testSeconds->setAccessibleName(tr("Test signal time limit"));
    m_testSeconds->setRange(PureSignalTestRun::kMinSeconds, PureSignalTestRun::kMaxSeconds);
    m_testSeconds->setValue(10);
    m_testSeconds->setSuffix(QStringLiteral(" s"));
    m_testIdentify = new QCheckBox(tr("CW ID"));
    m_testIdentify->setChecked(true);
    m_testIdentify->setToolTip(tr("Send your callsign when the signal ends"));
    m_testButton = new QPushButton(tr("Transmit"));
    markTxKeying(m_testButton);    // keys the transmitter
    m_testButton->setToolTip(
        tr("Checks the frequency is clear, then transmits on the current frequency and antenna"));
    m_testStatus = new QLabel;
    m_testStatus->setAccessibleName(tr("Test signal status"));
    test->addWidget(m_testMode);
    test->addWidget(m_testPower);
    test->addWidget(m_testSeconds);
    test->addWidget(m_testIdentify);
    test->addWidget(m_testButton);
    test->addWidget(m_testStatus, 1);
    root->addLayout(test);
    connect(m_testButton, &QPushButton::clicked, this, &PureSignalDialog::startOrStopTest);

    m_testTimer = new QTimer(this);
    m_testTimer->setInterval(kTestTickMs);
    connect(m_testTimer, &QTimer::timeout, this, &PureSignalDialog::tickTest);

    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshMs);
    connect(m_timer, &QTimer::timeout, this, &PureSignalDialog::refresh);
    m_timer->start();
    refresh();
}

PureSignalDialog::~PureSignalDialog()
{
    stopTest();
}

void PureSignalDialog::hideEvent(QHideEvent* event)
{
    // Nothing keys behind a window that isn't there. Not when it's only minimized.
    if (!event->spontaneous())
        stopTest();
    PersistentDialog::hideEvent(event);
}

void PureSignalDialog::startOrStopTest()
{
    if (m_test.active()) {
        stopTest();
        return;
    }
    if (!m_radio || !m_radio->isConnected())
        return;
    IRadioBackend* backend = m_radio->backend();
    const QString state = backend
        ? backend->healthSnapshot().values.value(QStringLiteral("psState")).toString()
        : QString();

    PureSignalTestRun::Config config;
    config.mode = static_cast<PureSignalTestRun::Mode>(m_testMode->currentData().toInt());
    config.seconds = m_testSeconds->value();
    config.psOn = !state.isEmpty() && state != QLatin1String("off");
    config.identify = m_testIdentify->isChecked();
    config.callsign = m_radio->callsign();
    carryOut(m_test.start(config, nowMs()));
    if (!m_test.active()) {
        testFinished();
        return;
    }
    TransmitModel& tx = m_radio->transmitModel();
    m_savedTunePower = tx.tunePower();
    tx.setTunePower(m_testPower->value());
    m_testButton->setText(tr("Stop"));
    m_testStatus->setText(tr("Listening"));
    m_testTimer->start();
}

void PureSignalDialog::tickTest()
{
    if (!m_radio || !m_radio->isConnected()) {
        stopTest();
        return;
    }
    IRadioBackend* backend = m_radio->backend();
    const QMap<QString, QVariant> values = backend ? backend->healthSnapshot().values
                                                   : QMap<QString, QVariant>{};
    PureSignalTestRun::Inputs in;
    const SliceModel* slice = m_radio->txSlice();
    in.levelDbm = slice ? m_radio->meterModel().sLevelForSlice(slice->sliceId())
                        : m_radio->meterModel().sLevelIfLive();
    in.keyed = m_radio->transmitModel().isTuning();
    in.correcting = values.value(QStringLiteral("psState")).toString()
                    == QLatin1String("correcting");
    in.calibrations = values.value(QStringLiteral("psCalibrations")).toInt();
    const PureSignalTestRun::Phase before = m_test.phase();
    carryOut(m_test.tick(nowMs(), in));
    if (!m_test.active()) {
        testFinished();
        return;
    }
    if (m_test.phase() != before) {
        m_testStatus->setText(m_test.phase() == PureSignalTestRun::Phase::Identifying
                                  ? tr("Sending ID")
                                  : tr("Transmitting"));
    }
}

void PureSignalDialog::stopTest()
{
    if (!m_test.active())
        return;
    carryOut(m_test.stop(nowMs()));
    testFinished();
}

void PureSignalDialog::carryOut(const QVector<PureSignalTestRun::Command>& commands)
{
    if (!m_radio)
        return;
    TransmitModel& tx = m_radio->transmitModel();
    for (const PureSignalTestRun::Command command : commands) {
        switch (command) {
        case PureSignalTestRun::Command::KeyTwoTone:
            tx.startTwoToneTune();
            break;
        case PureSignalTestRun::Command::KeyCarrier:
            tx.startTune();
            break;
        case PureSignalTestRun::Command::Unkey:
            tx.stopTune();
            break;
        case PureSignalTestRun::Command::BypassCorrection:
            m_radio->setPureSignalBypass(true);
            break;
        case PureSignalTestRun::Command::UseCorrection:
            m_radio->setPureSignalBypass(false);
            break;
        }
    }
}

void PureSignalDialog::testFinished()
{
    m_testTimer->stop();
    if (m_radio && m_savedTunePower >= 0)
        m_radio->transmitModel().setTunePower(m_savedTunePower);
    m_savedTunePower = -1;
    m_testButton->setText(tr("Transmit"));
    m_testStatus->setText(m_test.result());
}

void PureSignalDialog::refresh()
{
    if (!isVisible() || !m_radio)
        return;
    IRadioBackend* backend = m_radio->backend();
    if (!backend)
        return;
    const QMap<QString, QVariant> values = backend->healthSnapshot().values;
    const auto text = [&values](const char* key) {
        const QString v = values.value(QString::fromLatin1(key)).toString();
        return v.isEmpty() ? QStringLiteral("-") : v;
    };
    m_state->setText(text("psState"));
    m_level->setText(feedbackAdvice(values.value(QStringLiteral("psFeedbackLevel")).toInt()));
    m_gain->setText(text("txLnaGainDb") + tr(" dB"));
    m_calibrations->setText(tr("%1 of %2").arg(text("psCalibrations"), text("psAttempts")));
    m_peaks->setText(tr("%1 / %2 dBFS").arg(text("psFeedbackPeakDbfs"), text("psReferencePeakDbfs")));
    m_lastFailure->setText(text("psLastFailure"));

    const PureSignalCurves curves = backend->pureSignalCurves();
    if (curves.calibration != m_curves.calibration)
        showCurves(curves);
}

void PureSignalDialog::showCurves(const PureSignalCurves& curves)
{
    m_curves = curves;
    m_amplitude->setSamples(curves.amplitude);
    m_phase->setSamples(curves.phaseDeg);
    m_phase->setLine(curves.correctionPhaseDeg);
    m_correction->setLine(curves.correctionGain);
    m_hold->setEnabled(curves.valid());
    m_curveNote->setText(curves.valid() ? tr("Curve from calibration %1").arg(curves.calibration)
                                        : tr("No calibration yet"));
}

void PureSignalDialog::holdCurves()
{
    m_held = m_curves;
    m_amplitude->setHeld(m_held.amplitude);
    m_phase->setHeld(m_held.phaseDeg);
    m_correction->setHeld(m_held.correctionGain);
    m_clearHeld->setEnabled(true);
    m_hold->setText(tr("Hold curve (%1 held)").arg(m_held.calibration));
}

void PureSignalDialog::clearHeld()
{
    m_held = {};
    m_amplitude->setHeld({});
    m_phase->setHeld({});
    m_correction->setHeld({});
    m_clearHeld->setEnabled(false);
    m_hold->setText(tr("Hold curve"));
}

} // namespace AetherSDR

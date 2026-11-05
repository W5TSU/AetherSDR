#pragma once

#include "core/PureSignalTestRun.h"
#include "core/backends/PureSignalCurves.h"
#include "gui/PersistentDialog.h"

#include <QPointer>

class QCheckBox;
class QComboBox;
class QHideEvent;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;

namespace AetherSDR {

class PureSignalCurveWidget;
class RadioModel;

// What PureSignal is doing while it runs: the status it reports, the amplifier curve
// it measured and the correction it applies. Only the test signal keys anything, and
// only from its button.
class PureSignalDialog : public PersistentDialog {
    Q_OBJECT
public:
    explicit PureSignalDialog(RadioModel* radio, QWidget* parent = nullptr);
    ~PureSignalDialog() override;

    // The curves to draw. Public so a test can draw without a radio.
    void showCurves(const PureSignalCurves& curves);

protected:
    void hideEvent(QHideEvent* event) override;

private:
    void refresh();
    void startOrStopTest();
    void tickTest();
    void stopTest();
    void carryOut(const QVector<PureSignalTestRun::Command>& commands);
    void testFinished();
    void holdCurves();
    void clearHeld();

    QPointer<RadioModel> m_radio;
    QTimer* m_timer = nullptr;
    QLabel* m_state = nullptr;
    QLabel* m_level = nullptr;
    QLabel* m_gain = nullptr;
    QLabel* m_calibrations = nullptr;
    QLabel* m_peaks = nullptr;
    QLabel* m_lastFailure = nullptr;
    QLabel* m_curveNote = nullptr;
    PureSignalCurveWidget* m_amplitude = nullptr;
    PureSignalCurveWidget* m_phase = nullptr;
    PureSignalCurveWidget* m_correction = nullptr;
    QPushButton* m_hold = nullptr;
    QPushButton* m_clearHeld = nullptr;
    PureSignalCurves m_curves;
    PureSignalCurves m_held;

    QComboBox* m_testMode = nullptr;
    QSpinBox* m_testPower = nullptr;
    QSpinBox* m_testSeconds = nullptr;
    QCheckBox* m_testIdentify = nullptr;
    QPushButton* m_testButton = nullptr;
    QLabel* m_testStatus = nullptr;
    QTimer* m_testTimer = nullptr;
    PureSignalTestRun m_test;
    int m_savedTunePower = -1;
};

} // namespace AetherSDR

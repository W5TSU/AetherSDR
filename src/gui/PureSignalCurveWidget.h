#pragma once

#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <utility>

namespace AetherSDR {

// One PureSignal plot: measured samples as dots, a fitted curve as a line, and an
// optional held curve to compare against. X runs 0..1; Y fits what's drawn.
class PureSignalCurveWidget : public QWidget {
    Q_OBJECT
public:
    enum class YAxis {
        Level,       // 0..1, with the straight line a linear amplifier would give
        AroundZero,  // symmetric about 0, at least +/-minSpan
        Fitted,      // the data's own range, at least minSpan wide
    };

    PureSignalCurveWidget(const QString& title, const QString& xLabel, const QString& yLabel,
                          YAxis yAxis, double minSpan, QWidget* parent = nullptr);

    void setSamples(const QVector<QPointF>& points);
    void setLine(const QVector<QPointF>& points);
    void setHeld(const QVector<QPointF>& points);

    // The Y range for these points.
    static std::pair<double, double> yRange(YAxis yAxis, double minSpan,
                                            const QVector<QPointF>& points);

    [[nodiscard]] QSize minimumSizeHint() const override { return {220, 170}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_title;
    QString m_xLabel;
    QString m_yLabel;
    YAxis m_yAxis;
    double m_minSpan;
    QVector<QPointF> m_samples;
    QVector<QPointF> m_line;
    QVector<QPointF> m_held;
};

} // namespace AetherSDR

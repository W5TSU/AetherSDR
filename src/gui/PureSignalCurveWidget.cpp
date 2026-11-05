#include "PureSignalCurveWidget.h"

#include "core/ThemeManager.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

PureSignalCurveWidget::PureSignalCurveWidget(const QString& title, const QString& xLabel,
                                             const QString& yLabel, YAxis yAxis, double minSpan,
                                             QWidget* parent)
    : QWidget(parent)
    , m_title(title)
    , m_xLabel(xLabel)
    , m_yLabel(yLabel)
    , m_yAxis(yAxis)
    , m_minSpan(minSpan)
{
    setAccessibleName(title);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PureSignalCurveWidget::setSamples(const QVector<QPointF>& points)
{
    m_samples = points;
    update();
}

void PureSignalCurveWidget::setLine(const QVector<QPointF>& points)
{
    m_line = points;
    update();
}

void PureSignalCurveWidget::setHeld(const QVector<QPointF>& points)
{
    m_held = points;
    update();
}

std::pair<double, double> PureSignalCurveWidget::yRange(YAxis yAxis, double minSpan,
                                                        const QVector<QPointF>& points)
{
    if (yAxis == YAxis::Level)
        return {0.0, 1.0};
    double low = 0.0;
    double high = 0.0;
    bool any = false;
    for (const QPointF& p : points) {
        if (!std::isfinite(p.y()))
            continue;
        low = any ? std::min(low, p.y()) : p.y();
        high = any ? std::max(high, p.y()) : p.y();
        any = true;
    }
    if (yAxis == YAxis::AroundZero) {
        const double reach = std::max({std::abs(low), std::abs(high), minSpan}) * 1.1;
        return {-reach, reach};
    }
    const double middle = (low + high) / 2.0;
    const double half = std::max(high - low, minSpan) / 2.0 * 1.1;
    return {middle - half, middle + half};
}

void PureSignalCurveWidget::paintEvent(QPaintEvent*)
{
    auto& theme = ThemeManager::instance();
    const QColor background = theme.color(this, QStringLiteral("color.background.spectrum"));
    const QColor grid = theme.color(this, QStringLiteral("color.border.subtle"));
    const QColor text = theme.color(this, QStringLiteral("color.text.secondary"));
    const QColor sample = theme.color(this, QStringLiteral("color.accent.bright"));
    const QColor line = theme.color(this, QStringLiteral("color.accent.warning"));
    const QColor held = theme.color(this, QStringLiteral("color.text.primary"));

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), background);

    const QFontMetrics fm(font());
    const qreal lineHeight = fm.height();
    const QRectF plot(fm.horizontalAdvance(QStringLiteral("-000.0")) + 6.0, lineHeight + 6.0,
                      std::max(1.0, width() - fm.horizontalAdvance(QStringLiteral("-000.0")) - 14.0),
                      std::max(1.0, height() - 2.0 * lineHeight - 14.0));

    // Everything drawn sets the range, so nothing runs off the plot.
    const auto [yLow, yHigh] = yRange(m_yAxis, m_minSpan, m_samples + m_line + m_held);
    const auto toPixel = [&, yLow = yLow, yHigh = yHigh](const QPointF& v) {
        const double top = yHigh > yLow ? yHigh - yLow : 1.0;
        return QPointF(plot.left() + std::clamp(v.x(), 0.0, 1.0) * plot.width(),
                       plot.bottom() - (std::clamp(v.y(), yLow, yHigh) - yLow) / top * plot.height());
    };

    p.setPen(QPen(grid, 1.0));
    for (int i = 0; i <= 4; ++i) {
        const double f = i / 4.0;
        p.drawLine(QPointF(plot.left() + f * plot.width(), plot.top()),
                   QPointF(plot.left() + f * plot.width(), plot.bottom()));
        p.drawLine(QPointF(plot.left(), plot.top() + f * plot.height()),
                   QPointF(plot.right(), plot.top() + f * plot.height()));
    }

    p.setPen(text);
    p.drawText(QRectF(0, 2, width(), lineHeight), Qt::AlignCenter,
               QStringLiteral("%1 (%2)").arg(m_title, m_yLabel));
    p.drawText(QRectF(0, height() - lineHeight - 2, width(), lineHeight), Qt::AlignCenter, m_xLabel);
    const int decimals = yHigh - yLow < 2.0 ? 2 : 1;
    const QRectF labels(0, 0, plot.left() - 3.0, height());
    p.drawText(QRectF(labels.left(), plot.top() - 2, labels.width(), lineHeight),
               Qt::AlignRight | Qt::AlignTop, QString::number(yHigh, 'f', decimals));
    p.drawText(QRectF(labels.left(), plot.bottom() - lineHeight + 2, labels.width(), lineHeight),
               Qt::AlignRight | Qt::AlignBottom, QString::number(yLow, 'f', decimals));

    if (m_yAxis == YAxis::Level) {
        p.setPen(QPen(grid.lighter(160), 1.0, Qt::DashLine));
        p.drawLine(toPixel({0.0, 0.0}), toPixel({1.0, 1.0}));
    }

    p.setClipRect(plot);
    const auto drawLine = [&](const QVector<QPointF>& points, const QPen& pen) {
        if (points.size() < 2)
            return;
        QPainterPath path(toPixel(points.first()));
        for (qsizetype i = 1; i < points.size(); ++i)
            path.lineTo(toPixel(points[i]));
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };

    p.setPen(Qt::NoPen);
    QColor heldDot = held;
    heldDot.setAlphaF(0.35f);
    p.setBrush(heldDot);
    for (const QPointF& v : m_held)
        p.drawEllipse(toPixel(v), 1.2, 1.2);
    p.setBrush(sample);
    for (const QPointF& v : m_samples)
        p.drawEllipse(toPixel(v), 1.2, 1.2);
    drawLine(m_line, QPen(line, 1.5));
}

} // namespace AetherSDR

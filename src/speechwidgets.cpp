#include "speechwidgets.h"

#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QStyleOptionButton>
#include <QtMath>

SymmetricLevelMeter::SymmetricLevelMeter(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setAccessibleName(tr("Microphone level"));
    setAccessibleDescription(tr("Mono microphone level, mirrored from the center outward."));
    setToolTip(tr("Microphone input level · both sides show the same mono signal"));
    m_release.setDuration(140);
    m_release.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_release, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_displayValue = value.toReal();
        update();
    });
}

void SymmetricLevelMeter::setValue(int value)
{
    value = qBound(0, value, 100);
    if (value == m_value) return;
    m_value = value;
    m_release.stop();
    // Reset immediately so stopped/cancelled capture cannot leave a lit meter.
    if (value == 0 || value >= m_displayValue || !isVisible()) {
        m_displayValue = value;
        update();
    } else {
        m_release.setStartValue(m_displayValue);
        m_release.setEndValue(qreal(value));
        m_release.start();
    }
}

void SymmetricLevelMeter::setDark(bool dark)
{
    if (m_dark == dark) return;
    m_dark = dark;
    update();
}

QSize SymmetricLevelMeter::sizeHint() const { return {156, 16}; }
QSize SymmetricLevelMeter::minimumSizeHint() const { return {48, 12}; }

void SymmetricLevelMeter::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal trackHeight = qMin(qreal(8), qMax(qreal(2), height() - 2.0));
    constexpr qreal centerGap = 4.0;
    constexpr qreal segmentGap = 2.0;
    const qreal halfWidth = (width() - 2.0 - centerGap) / 2.0;
    if (halfWidth <= 0) return;
    const int segments = qBound(1, int((halfWidth + segmentGap) / 5.0), 18);
    const qreal segmentWidth = (halfWidth - segmentGap * (segments - 1)) / segments;
    const qreal center = width() / 2.0;
    const qreal top = (height() - trackHeight) / 2.0;
    const qreal level = qBound(qreal(0), m_displayValue, qreal(100)) * segments / 100.0;
    const QColor inactive(m_dark ? "#41464d" : "#d8dde2");
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < segments; ++i) {
        const qreal zone = qreal(i + 1) / segments;
        const QColor active = zone > 0.9 ? QColor("#df665f")
                            : zone >= 0.7 ? QColor("#d4a64d")
                                          : QColor("#50a77b");
        qreal amount = qBound(qreal(0), level - i, qreal(1));
        if (!isEnabled()) amount *= 0.4;
        const auto channel = [amount](int off, int on) { return qRound(off + (on - off) * amount); };
        painter.setBrush(QColor(channel(inactive.red(), active.red()),
                                channel(inactive.green(), active.green()),
                                channel(inactive.blue(), active.blue())));
        const qreal offset = centerGap / 2.0 + i * (segmentWidth + segmentGap);
        // Both sides use the same segment and color. Only the outer segments
        // become amber/red; a high signal never recolors the whole meter red.
        painter.drawRoundedRect(QRectF(center + offset, top, segmentWidth, trackHeight), 1.2, 1.2);
        painter.drawRoundedRect(QRectF(center - offset - segmentWidth, top, segmentWidth, trackHeight), 1.2, 1.2);
    }
}

AnimatedToggleButton::AnimatedToggleButton(const QString &text, QWidget *parent)
    : QPushButton(text, parent)
{
    setCheckable(true);
    m_transition.setDuration(120);
    m_transition.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_transition, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_checkedProgress = value.toReal();
        update();
    });
    connect(this, &QPushButton::toggled, this, [this](bool checked) {
        m_transition.stop();
        const qreal target = checked ? 1.0 : 0.0;
        if (!isVisible()) {
            m_checkedProgress = target;
            update();
            return;
        }
        m_transition.setStartValue(m_checkedProgress);
        m_transition.setEndValue(target);
        m_transition.start();
    });
}

void AnimatedToggleButton::setDark(bool dark)
{
    Q_UNUSED(dark);
    // The application palette and style sheet own both light and dark colors.
    update();
}

void AnimatedToggleButton::paintEvent(QPaintEvent *)
{
    QStyleOptionButton normal;
    initStyleOption(&normal);
    const bool physicallyPressed = isDown();
    normal.state &= ~(QStyle::State_On | QStyle::State_Off | QStyle::State_Sunken);
    normal.state |= QStyle::State_Off;
    if (physicallyPressed) normal.state |= QStyle::State_Sunken;

    QStyleOptionButton pressed = normal;
    pressed.state &= ~QStyle::State_Off;
    pressed.state |= QStyle::State_On | QStyle::State_Sunken;

    QPainter painter(this);
    if (m_checkedProgress <= 0.001) {
        style()->drawControl(QStyle::CE_PushButton, &normal, &painter, this);
        return;
    }
    if (m_checkedProgress >= 0.999) {
        style()->drawControl(QStyle::CE_PushButton, &pressed, &painter, this);
        return;
    }

    // Render through the active style (including the application's QSS) so
    // borders, focus indication, labels and disabled colors remain consistent.
    const qreal scale = devicePixelRatioF();
    const QSize pixels(qCeil(width() * scale), qCeil(height() * scale));
    QPixmap normalFrame(pixels), pressedFrame(pixels);
    normalFrame.setDevicePixelRatio(scale);pressedFrame.setDevicePixelRatio(scale);
    normalFrame.fill(Qt::transparent);pressedFrame.fill(Qt::transparent);
    {
        QPainter frame(&normalFrame);
        style()->drawControl(QStyle::CE_PushButton, &normal, &frame, this);
    }
    {
        QPainter frame(&pressedFrame);
        style()->drawControl(QStyle::CE_PushButton, &pressed, &frame, this);
    }
    painter.drawPixmap(0, 0, normalFrame);
    painter.setOpacity(m_checkedProgress);
    painter.drawPixmap(0, 0, pressedFrame);
}

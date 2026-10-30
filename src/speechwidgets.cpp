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
    : QPushButton(text, parent), m_label(text)
{
    setCheckable(true);
    setAccessibleName(text);
    setAccessibleDescription(tr("Off"));
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_transition.setDuration(120);
    m_transition.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_transition, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_checkedProgress = value.toReal();
        update();
    });
    connect(this, &QPushButton::toggled, this, [this](bool checked) {
        setAccessibleDescription(checked ? tr("On") : tr("Off"));
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
    m_dark = dark;
    update();
}

QSize AnimatedToggleButton::sizeHint() const
{
    QFont labelFont = font();labelFont.setWeight(QFont::DemiBold);
    QFontMetrics metrics(labelFont);
    const int width = metrics.horizontalAdvance(m_label);
    return QSize(width + 28, qMax(36, metrics.height() + 16));
}

QSize AnimatedToggleButton::minimumSizeHint() const { return sizeHint(); }

void AnimatedToggleButton::paintEvent(QPaintEvent *)
{
    const QColor off(m_dark ? "#292a2e" : "#ffffff");
    const QColor on(m_dark ? "#1b4776" : "#b3d4ff");
    const auto mix = [](const QColor &a, const QColor &b, qreal t) {
        return QColor(qRound(a.red() + (b.red() - a.red()) * t),
                      qRound(a.green() + (b.green() - a.green()) * t),
                      qRound(a.blue() + (b.blue() - a.blue()) * t));
    };
    QColor background = mix(off, on, m_checkedProgress);
    QColor border = mix(QColor(m_dark ? "#70757f" : "#9aa0a9"),
                        QColor(m_dark ? "#6aa5e3" : "#2d6fba"), m_checkedProgress);
    if (isEnabled() && (underMouse() || isDown()))
        background = mix(background, QColor(m_dark ? "#ffffff" : "#000000"), isDown() ? 0.12 : 0.05);
    // Select the foreground from the animated fill so it stays legible in the transition.
    QColor foreground(qGray(background.rgb()) > 127 ? "#1d2024" : "#ffffff");
    if (!isEnabled()) {
        const QColor window = palette().color(QPalette::Window);
        background = mix(background, window, 0.45);
        border = mix(border, window, 0.45);
        foreground = mix(foreground, background, 0.35);
    }
    QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(border, 1));painter.setBrush(background);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5,0.5,-0.5,-0.5), 7, 7);
    QFont labelFont = font();labelFont.setWeight(QFont::DemiBold);painter.setFont(labelFont);
    painter.setPen(foreground);painter.drawText(rect().translated(0,isDown()?1:0), Qt::AlignCenter, text());
    if (hasFocus()) {
        painter.setPen(QPen(foreground, 1, Qt::DotLine));painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(4.5,4.5,-4.5,-4.5), 3, 3);
    }
}

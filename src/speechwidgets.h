#pragma once

#include <QPushButton>
#include <QVariantAnimation>
#include <QWidget>

// One mono signal, mirrored around the midpoint; this is not a stereo meter.
class SymmetricLevelMeter final : public QWidget {
    Q_OBJECT
public:
    explicit SymmetricLevelMeter(QWidget *parent = nullptr);
    void setValue(int value);
    int value() const { return m_value; }
    void setDark(bool dark);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int m_value = 0;
    qreal m_displayValue = 0;
    bool m_dark = false;
    QVariantAnimation m_release;
};

// Native button geometry, accessibility and keyboard handling, with a short
// crossfade between the style's normal and persistently pressed states.
class AnimatedToggleButton final : public QPushButton {
    Q_OBJECT
public:
    explicit AnimatedToggleButton(const QString &text, QWidget *parent = nullptr);
    void setDark(bool dark);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    qreal m_checkedProgress = 0;
    QVariantAnimation m_transition;
};

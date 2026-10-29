#pragma once
#include <QPlainTextEdit>
class LineNumberArea;
class NumberedEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit NumberedEditor(QWidget *parent = nullptr);
    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);
    void setReadingRange(int start, int length, int wordStart = -1, int wordLength = 0, bool follow = true);
    void clearReadingRange();
protected:
    void resizeEvent(QResizeEvent *) override;
private:
    QWidget *m_gutter;
    int m_lastFollowBlock = -1;
    void updateGutterWidth();
};

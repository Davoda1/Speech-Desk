#pragma once
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QVector>

struct WordCue {
    int start = 0; // UTF-16 offset relative to request text
    int length = 0;
    qint64 startMs = 0;
    qint64 endMs = 0;
};
struct SpeechSegment {
    QString text;
    int start = 0; // UTF-16 offset in editor
    int length = 0;
};
Q_DECLARE_METATYPE(WordCue)
Q_DECLARE_METATYPE(QVector<WordCue>)

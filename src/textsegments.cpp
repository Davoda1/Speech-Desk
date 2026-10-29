#include "textsegments.h"
#include <QTextBoundaryFinder>
#include <algorithm>

QVector<SpeechSegment> splitSpeechText(const QString &text, int maxChars, int baseOffset)
{
    QVector<SpeechSegment> result;
    maxChars = qBound(80, maxChars, 8000);
    auto addRange = [&](int from, int to) {
        while (from < to) {
            while (from < to && text[from].isSpace()) ++from;
            if (from >= to) break;
            int end = qMin(to, from + maxChars);
            if (end < to) {
                int cut = end;
                while (cut > from + maxChars / 2 && !text[cut].isSpace()) --cut;
                if (cut > from + maxChars / 2) end = cut;
                if (end > from && text[end - 1].isHighSurrogate()) --end;
            }
            int trimmed = end;
            while (trimmed > from && text[trimmed-1].isSpace()) --trimmed;
            if (trimmed > from) result.push_back({text.mid(from, trimmed-from), baseOffset+from, trimmed-from});
            from = end;
        }
    };
    QTextBoundaryFinder finder(QTextBoundaryFinder::Sentence, text);
    int start = 0;
    for (int end = finder.toNextBoundary(); end >= 0; end = finder.toNextBoundary()) {
        // Paragraphs are independently playable even without terminal punctuation.
        int part = start;
        for (int i = start; i < end; ++i) if (text[i] == '\n') { addRange(part, i); part = i + 1; }
        addRange(part, end);
        start = end;
    }
    if (start < text.size()) addRange(start, text.size());
    return result;
}

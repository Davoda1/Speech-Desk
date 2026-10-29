#pragma once
#include "speechtypes.h"
QVector<SpeechSegment> splitSpeechText(const QString &text, int maxChars = 500, int baseOffset = 0);

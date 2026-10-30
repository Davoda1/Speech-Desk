#pragma once

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

// Provider retirement rules apply to the official endpoint, never to a private
// service which happens to use the same model names or compatible HTTP API.
namespace ProviderLifecycle {
inline QDate utcToday() { return QDateTime::currentDateTimeUtc().date(); }
inline QDate legacySpeechShutdown() { return QDate(2027, 1, 6); }
inline bool isOfficialOpenAi(const QJsonObject &profile)
{
    return QUrl(profile.value("baseUrl").toString()).host().compare("api.openai.com", Qt::CaseInsensitive) == 0;
}
inline bool isLegacySpeechModel(const QString &model)
{
    return model == "tts-1" || model == "tts-1-hd" || model.startsWith("tts-1-")
        || model == "gpt-4o-mini-tts" || model.startsWith("gpt-4o-mini-tts-");
}
inline bool isRealtimeModel(const QString &model)
{
    static const QRegularExpression supported("^gpt-realtime-2\\.1(?:-mini)?(?:-\\d{4}-\\d{2}-\\d{2})?$");
    return supported.match(model).hasMatch();
}
inline bool modelAllowed(const QJsonObject &profile, const QString &model, QDate day = utcToday())
{
    return !(isOfficialOpenAi(profile) && day >= legacySpeechShutdown() && isLegacySpeechModel(model));
}
inline QJsonArray modelChoices(const QJsonObject &profile, const QJsonArray &models, QDate day = utcToday())
{
    QJsonArray result;
    for (const auto &model : models) {
        const auto id = model.isString() ? model.toString() : model.toObject().value("id").toString();
        if (modelAllowed(profile, id, day)) result.append(model);
    }
    return result;
}
inline bool usesRealtime(const QJsonObject &profile)
{
    return (profile.value("provider").toString() == "openai" || isOfficialOpenAi(profile))
        && isRealtimeModel(profile.value("model").toString());
}
}

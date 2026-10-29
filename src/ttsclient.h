#pragma once

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSet>
#include <QUrl>
#include <functional>
#include "speechtypes.h"

class QNetworkReply;

// Async, single-operation speech client. Starting a probe or synthesis cancels
// the previous operation; cancellation never emits stale audio or probe results.
class TtsClient final : public QObject {
    Q_OBJECT
public:
    explicit TtsClient(QObject *parent = nullptr);
    ~TtsClient() override;
    void setProfile(const QJsonObject &profile);
    QJsonObject profile() const;
    void probe();
    void synthesize(const QString &text);
    void cancel();

signals:
    void status(const QString &message);
    void error(const QString &message);
    void capabilities(const QJsonObject &info);
    void audioReady(QByteArray audio, QString format, QVector<WordCue> cues);
    void probeFinished(QJsonObject info);

private:
    struct Response {
        QByteArray data;
        QByteArray contentType;
        int httpStatus = 0;
        QString failure;
        int sampleRate = 0;
        int channels = 0;
    };
    using Completion = std::function<void(Response)>;
    QNetworkAccessManager m_network;
    QJsonObject m_profile;
    QSet<QNetworkReply *> m_pending;
    quint64 m_generation = 0;
    bool m_destroying = false;
    QString m_detectedAdapter;

    QUrl baseUrl(QString *failure = nullptr) const;
    QUrl endpoint(const QString &path, QString *failure = nullptr) const;
    void request(const QUrl &url, const QByteArray &body, bool get,
                 quint64 generation, Completion completion, int timeoutOverride = 0);
    void consume(Response response, const QString &text, quint64 generation,
                 QVector<WordCue> inheritedCues = {}, bool isAudioDownload = false);
    void consumeJson(const QJsonObject &object, const QString &text, quint64 generation);
    void consumeSse(const QByteArray &data, const QString &text, quint64 generation);
    void deliverAudio(QByteArray audio, QString format, QVector<WordCue> cues,
                      int sampleRate = 0, int channels = 0);
    static QVector<WordCue> readCues(const QJsonObject &object, const QString &path,
                                    const QString &text);
};

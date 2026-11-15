#pragma once

#include <QObject>
#include <QJsonObject>
#include <QByteArray>
#include <memory>

class QProcess;
class QTemporaryDir;
class QTimer;

// One bounded, cancellable offline worker. It returns the same complete WAV
// sections used by the existing speech playback and export pipeline.
class LocalTts final : public QObject {
    Q_OBJECT
public:
    explicit LocalTts(QObject *parent = nullptr);
    ~LocalTts() override;
    void synthesize(const QJsonObject &profile, const QString &text);
    void cancel();
    static QString assetRoot();

signals:
    void audioReady(QByteArray audio);
    void error(QString message);
    void status(QString message);

private:
    QProcess *m_process = nullptr;
    QTimer *m_timeout;
    std::unique_ptr<QTemporaryDir> m_work;
    QByteArray m_stdout;
    qsizetype m_logBytes = 0;
    bool m_completed = false;
    QString m_output;
    void consumeOutput();
    void fail(const QString &message);
};

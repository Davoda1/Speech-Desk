#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <memory>

class QTemporaryDir;

// Offline recognition through bundled native engines. No shell or network is used.
// A live transcript is provisional; the final signal replaces it after the
// complete recording has been transcribed when the microphone stops.
class SttService final : public QObject {
    Q_OBJECT
public:
    explicit SttService(QObject *parent = nullptr);
    ~SttService() override;

    void setResourceRoot(const QString &root);
    void setOptions(const QString &modelId = QStringLiteral("whistle"),
                    const QString &language = QStringLiteral("auto"), int threads = 4);
    void requestDevices();
    void transcribeFile(const QString &path);
    void startRecording(const QString &deviceId, bool live);
    void stopRecording();
    void cancel();
    bool busy() const { return m_busy; }

signals:
    void devicesReady(QJsonArray devices);
    void devicesError(QString message);
    void status(QString message);
    void error(QString message);
    void transcript(QString text, bool final);
    void levelChanged(double peak, double seconds);
    void recordingChanged(bool active);
    void finished();

private:
    QString resourceRoot() const;
    QString enginePath(const QString &name) const;
    QString modelPath() const;
    bool beginTask();
    void fail(const QString &message);
    void finishTask();
    void resetTask();
    void stopProcess(QProcess *&process);
    QProcess *makeProcess(const QString &program, int timeoutMs);
    void consumeRecordingOutput();
    void startInference(const QString &wavPath, bool final, qint64 startFrame = 0,
                        qint64 endFrame = 0);
    void updateLive();
    bool writeSnapshot(qint64 firstFrame, qint64 lastFrame, const QString &target);
    QString parseTranscript(const QByteArray &output, const QString &jsonPath,
                            QString *parseError) const;
    static QString helperFailure(const QByteArray &output, const QByteArray &errors,
                                 const QString &fallback);
    static QString appendWithoutOverlap(const QString &before, const QString &after);

    QString m_root;
    QString m_model = QStringLiteral("whistle");
    QString m_language = QStringLiteral("auto");
    int m_threads = 4;
    QString m_jobRoot, m_jobModel, m_jobModelPath, m_jobLanguage;
    int m_jobThreads = 4;
    quint64 m_generation = 0;
    bool m_busy = false;
    bool m_recording = false;
    bool m_stopRequested = false;
    bool m_live = false;
    bool m_captureStarted = false;
    QProcess *m_devices = nullptr;
    QProcess *m_capture = nullptr;
    QProcess *m_converter = nullptr;
    QProcess *m_inference = nullptr;
    QTimer m_liveTimer;
    std::shared_ptr<QTemporaryDir> m_temp;
    QString m_recordingPath;
    QByteArray m_recordEvents;
    QString m_captureFailure;
    QString m_committedText;
    qint64 m_committedFrame = 0;
    qint64 m_lastLiveEnd = 0;
};

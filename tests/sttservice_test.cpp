// Optional native engine integration check.
// Usage: sttservice_test RESOURCE_ROOT AUDIO_FILE [whistle|tiny|base|small]
#include "sttservice.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QtEndian>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

// A deliberately synthetic microphone transport fixture. It exercises the
// parent's process, WAV snapshot and stop protocol with real model inference;
// it does not claim to test an actual microphone or operating-system driver.
static int captureFixture(QCoreApplication &app) {
#ifdef Q_OS_UNIX
    QFile input(qEnvironmentVariable("SPEECHDESK_TEST_CAPTURE_WAV"));
    if (!input.open(QIODevice::ReadOnly)) return 4;
    QByteArray header = input.read(44);
    const QByteArray samples = input.readAll();
    if (header.size() != 44 || samples.isEmpty()) return 4;
    const int targetIndex = app.arguments().indexOf(QStringLiteral("--output")) + 1;
    QFile output(app.arguments().value(targetIndex));
    if (!output.open(QIODevice::WriteOnly)) return 4;
    qToLittleEndian<quint32>(36, header.data() + 4);
    qToLittleEndian<quint32>(0, header.data() + 40);
    output.write(header);
    output.flush();
    qint64 count = 0;
    QTextStream(stdout) << "{\"type\":\"started\"}\n" << Qt::flush;
    QTimer pump;
    QObject::connect(&pump, &QTimer::timeout, [&] {
        QByteArray pcm(3200, '\0');
        const qint64 offset = count % samples.size();
        for (int i = 0; i < pcm.size(); ++i) pcm[i] = samples.at((offset + i) % samples.size());
        if (output.write(pcm) != pcm.size() || !output.flush()) app.exit(5);
        count += pcm.size();
        QTextStream(stdout) << "{\"type\":\"level\",\"peak\":0.4,\"seconds\":"
                            << double(count) / 32000.0 << "}\n" << Qt::flush;
    });
    QSocketNotifier inputSignal(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&inputSignal, &QSocketNotifier::activated, &app, [&] {
        char buffer[32];
        const auto result = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (result >= 0) app.quit();
    });
    QTimer::singleShot(12000, &app, &QCoreApplication::quit);
    pump.start(100);
    const int code = app.exec();
    qToLittleEndian<quint32>(quint32(count + 36), header.data() + 4);
    qToLittleEndian<quint32>(quint32(count), header.data() + 40);
    output.seek(0);
    output.write(header);
    output.close();
    QTextStream(stdout) << "{\"type\":\"stopped\",\"duration\":" << double(count) / 32000.0
                        << "}\n" << Qt::flush;
    return code;
#else
    Q_UNUSED(app);
    return 4;
#endif
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == QStringLiteral("record")
        && qEnvironmentVariableIsSet("SPEECHDESK_TEST_CAPTURE_WAV")) return captureFixture(app);
    if (app.arguments().size() < 3) return 2;
    SttService service;
    service.setResourceRoot(app.arguments().at(1));
    int errors = 0, finalResults = 0, finishes = 0;
    QString lastError, finalText;
    QObject::connect(&service, &SttService::error, [&](const QString &message) {
        ++errors; lastError = message;
    });
    QObject::connect(&service, &SttService::transcript, [&](const QString &text, bool final) {
        if (final) { ++finalResults; finalText = text; }
    });
    QObject::connect(&service, &SttService::finished, [&] { ++finishes; });
    service.setOptions(QStringLiteral("whistle"), QStringLiteral("hu"));
    service.transcribeFile(app.arguments().at(2));
    const bool unsupportedLanguage = errors == 1 && finishes == 1 && !service.busy();
    service.setOptions(app.arguments().value(3, QStringLiteral("whistle")), QStringLiteral("en"), 4);
    service.transcribeFile(QStringLiteral("/nonexistent/speechdesk-no-audio.wav"));
    const bool missingFile = errors == 2 && finishes == 2 && !service.busy();
    errors = 0; lastError.clear();
    QEventLoop run;
    QObject::connect(&service, &SttService::finished, &run, &QEventLoop::quit);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &run, &QEventLoop::quit);
    timeout.start(60000);
    QElapsedTimer clock;
    clock.start();
    service.transcribeFile(app.arguments().at(2));
    if (service.busy()) run.exec();
    const bool nativeSuccess = !service.busy() && errors == 0 && finalResults == 1 && !finalText.isEmpty();
    const qint64 elapsed = clock.elapsed();
    if (service.busy()) service.cancel();
    const int oldResults = finalResults;
    service.transcribeFile(app.arguments().at(2));
    service.cancel();
    QEventLoop settle;
    QTimer::singleShot(600, &settle, &QEventLoop::quit);
    settle.exec();
    const bool cancelClean = !service.busy() && finalResults == oldResults;
    bool livePreview = true, recordedFinal = true, liveFinal = true;
    const bool captureRequested = app.arguments().contains(QStringLiteral("--capture-fixture"));
#ifdef Q_OS_UNIX
    if (captureRequested) {
        QTemporaryDir fixture;
        QDir(fixture.path()).mkdir(QStringLiteral("bin"));
        QFile::link(app.arguments().at(1) + QStringLiteral("/models"), fixture.filePath(QStringLiteral("models")));
        const QDir realBin(app.arguments().at(1) + QStringLiteral("/bin"));
        for (const QString &file : realBin.entryList(QDir::Files))
            if (file != QStringLiteral("whistle-audio"))
                QFile::link(realBin.filePath(file), fixture.filePath(QStringLiteral("bin/") + file));
        QFile::link(QCoreApplication::applicationFilePath(), fixture.filePath(QStringLiteral("bin/whistle-audio")));
        const QString canonicalWav = fixture.filePath(QStringLiteral("canonical.wav"));
        QProcess convert;
        convert.start(realBin.filePath(QStringLiteral("whistle-audio")),
            {QStringLiteral("convert"), QStringLiteral("--input"), app.arguments().at(2),
             QStringLiteral("--output"), canonicalWav});
        const bool converted = convert.waitForFinished(10000) && convert.exitCode() == 0;
        qputenv("SPEECHDESK_TEST_CAPTURE_WAV", canonicalWav.toUtf8());
        service.setResourceRoot(fixture.path());
        int partials = 0;
        QObject::connect(&service, &SttService::transcript, [&](const QString &text, bool final) {
            if (!final && !text.isEmpty()) ++partials;
        });
        const auto recordCase = [&](bool live, int stopMs) {
            const int oldFinals = finalResults;
            const int oldErrors = errors;
            QEventLoop loop;
            const auto connection = QObject::connect(&service, &SttService::finished, &loop, &QEventLoop::quit);
            QTimer::singleShot(stopMs, &service, &SttService::stopRecording);
            QTimer limit;
            limit.setSingleShot(true);
            QObject::connect(&limit, &QTimer::timeout, &loop, &QEventLoop::quit);
            limit.start(20000);
            service.startRecording({}, live);
            if (service.busy()) loop.exec();
            QObject::disconnect(connection);
            const bool success = !service.busy() && errors == oldErrors && finalResults == oldFinals + 1;
            if (service.busy()) service.cancel();
            return success;
        };
        recordedFinal = converted && recordCase(false, 1600);
        liveFinal = converted && recordCase(true, 6500);
        livePreview = partials > 0;
        qunsetenv("SPEECHDESK_TEST_CAPTURE_WAV");
    }
#endif
    const bool ok = unsupportedLanguage && missingFile && nativeSuccess && cancelClean
        && livePreview && recordedFinal && liveFinal;
    const QJsonObject result{{QStringLiteral("ok"), ok},
        {QStringLiteral("unsupported_language"), unsupportedLanguage},
        {QStringLiteral("missing_file"), missingFile},
        {QStringLiteral("native_recognition"), nativeSuccess},
        {QStringLiteral("cancel_no_stale_result"), cancelClean},
        {QStringLiteral("synthetic_live_preview"), captureRequested ? QJsonValue(livePreview) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("synthetic_record_then_transcribe"), captureRequested ? QJsonValue(recordedFinal) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("synthetic_live_final"), captureRequested ? QJsonValue(liveFinal) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("transcript"), finalText},
        {QStringLiteral("elapsed_ms"), elapsed},
        {QStringLiteral("error"), lastError}};
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
    return ok ? 0 : 1;
}

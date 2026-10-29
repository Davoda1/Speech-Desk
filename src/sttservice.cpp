#include "sttservice.h"
#include "modelpacks.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
constexpr qint64 SampleRate = 16000;
constexpr qint64 BytesPerFrame = 2;
constexpr qint64 MaxFrames = SampleRate * 3600;
constexpr qint64 LiveWindowFrames = SampleRate * 24;
constexpr qint64 CommitWindowFrames = SampleRate * 20;
constexpr qint64 OverlapFrames = SampleRate / 2;
constexpr qsizetype OutputLimit = 16 * 1024 * 1024;

QString processDetail(QProcess *p, const QString &fallback) {
    if (p->property("timedOut").toBool())
        return QStringLiteral("The native speech process timed out. Try a smaller model or a shorter recording.");
    if (p->property("outputLimit").toBool())
        return QStringLiteral("The native speech process returned an unexpectedly large response.");
    return fallback;
}

QByteArray wavHeader(quint32 bytes) {
    QByteArray header(44, '\0');
    std::copy_n("RIFF", 4, header.data());
    qToLittleEndian<quint32>(bytes + 36, header.data() + 4);
    std::copy_n("WAVEfmt ", 8, header.data() + 8);
    qToLittleEndian<quint32>(16, header.data() + 16);
    qToLittleEndian<quint16>(1, header.data() + 20);
    qToLittleEndian<quint16>(1, header.data() + 22);
    qToLittleEndian<quint32>(16000, header.data() + 24);
    qToLittleEndian<quint32>(32000, header.data() + 28);
    qToLittleEndian<quint16>(2, header.data() + 32);
    qToLittleEndian<quint16>(16, header.data() + 34);
    std::copy_n("data", 4, header.data() + 36);
    qToLittleEndian<quint32>(bytes, header.data() + 40);
    return header;
}
}

SttService::SttService(QObject *parent) : QObject(parent) {
    m_liveTimer.setInterval(4000);
    connect(&m_liveTimer, &QTimer::timeout, this, &SttService::updateLive);
}

SttService::~SttService() {
    ++m_generation;
    m_liveTimer.stop();
    // Shutdown only: ensure native workers release their temporary files before
    // QTemporaryDir is destroyed, without allowing a recorder to survive the UI.
    for (QProcess *p : {m_devices, m_capture, m_converter, m_inference}) {
        if (!p) continue;
        disconnect(p, nullptr, this, nullptr);
        p->kill();
        p->waitForFinished(1000);
    }
}

void SttService::setResourceRoot(const QString &root) {
    m_root = QDir(root).absolutePath();
}

void SttService::setOptions(const QString &modelId, const QString &language, int threads) {
    m_model = modelId.trimmed().toLower();
    if (m_model.startsWith(QStringLiteral("whisper-"))) m_model.remove(0, 8);
    m_language = language.trimmed().toLower();
    if (m_language.isEmpty()) m_language = QStringLiteral("auto");
    m_threads = std::clamp(threads, 1, 32);
}

QString SttService::resourceRoot() const {
    if (!m_root.isEmpty()) return m_root;
    const QString overrideRoot = qEnvironmentVariable("SPEECHDESK_RESOURCES");
    if (!overrideRoot.isEmpty()) return QDir(overrideRoot).absolutePath();
    const QString local = QCoreApplication::applicationDirPath() + QStringLiteral("/resources");
    if (QDir(local).exists()) return local;
    const QString installed = QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("../lib/speechdesk/resources"));
    if (QDir(installed).exists()) return QDir::cleanPath(installed);
    return local;
}

QString SttService::enginePath(const QString &name) const {
    const QString root = m_busy ? m_jobRoot : resourceRoot();
#ifdef Q_OS_WIN
    return root + QStringLiteral("/bin/") + name + QStringLiteral(".exe");
#else
    return root + QStringLiteral("/bin/") + name;
#endif
}

QString SttService::modelPath() const {
    return m_jobModelPath;
}

bool SttService::beginTask() {
    if (m_busy) {
        emit status(QStringLiteral("Finish or cancel the current transcription first."));
        return false;
    }
    ++m_generation;
    m_busy = true;
    m_jobRoot = resourceRoot();
    m_jobModel = m_model;
    m_jobModelPath.clear();
    m_jobLanguage = m_language;
    m_jobThreads = m_threads;
    m_stopRequested = false;
    m_captureStarted = false;
    m_captureFailure.clear();
    m_recordEvents.clear();
    m_committedText.clear();
    m_committedFrame = m_lastLiveEnd = 0;
    const QStringList models{QStringLiteral("whistle"), QStringLiteral("tiny"),
                             QStringLiteral("base"), QStringLiteral("small")};
    if (models.contains(m_jobModel)) {
        const QString file = m_jobModel == QStringLiteral("whistle")
            ? QStringLiteral("whistle.cact") : QStringLiteral("ggml-%1-q5_1.bin").arg(m_jobModel);
        m_jobModelPath = m_jobRoot + QStringLiteral("/models/") + file;
    } else {
        m_jobModelPath = ModelPacks::resolve(m_jobModel);
        if (m_jobModelPath.isEmpty()) {
            fail(QStringLiteral("The selected dictation model is unavailable. Choose an embedded model or import its optional model pack in Settings."));
            return false;
        }
    }
    const QStringList whistleLanguages{QStringLiteral("auto"), QStringLiteral("en"),
        QStringLiteral("de"), QStringLiteral("fr"), QStringLiteral("es"),
        QStringLiteral("it"), QStringLiteral("nl"), QStringLiteral("pl")};
    if (m_jobModel == QStringLiteral("whistle") && !whistleLanguages.contains(m_jobLanguage)) {
        fail(QStringLiteral("Whistle supports English, German, French, Spanish, Italian, Dutch and Polish. Choose Whisper for other languages."));
        return false;
    }
    if (!QRegularExpression(QStringLiteral("^(auto|[a-z]{2,3})$"))
             .match(m_jobLanguage).hasMatch()) {
        fail(QStringLiteral("Select a valid recognition language."));
        return false;
    }
    const QStringList engines{QStringLiteral("whistle-audio"),
        m_jobModel == QStringLiteral("whistle") ? QStringLiteral("needle") : QStringLiteral("whisper-cli")};
    for (const QString &name : engines) {
        const QFileInfo executable(enginePath(name));
        if (!executable.isFile() || !executable.isExecutable()) {
            fail(QStringLiteral("The bundled %1 engine is missing or is not executable. Extract the complete application package.").arg(name));
            return false;
        }
    }
    const QFileInfo weights(modelPath());
    if (!weights.isFile() || weights.size() < 1024) {
        fail(QStringLiteral("The selected dictation model is missing. Extract the complete application package or import its optional model pack again."));
        return false;
    }
    m_temp = std::make_shared<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/speechdesk-stt-XXXXXX"));
    if (!m_temp->isValid()) {
        fail(QStringLiteral("Cannot create temporary audio files. Check the temporary directory and available disk space."));
        return false;
    }
    return true;
}

QProcess *SttService::makeProcess(const QString &program, int timeoutMs) {
    auto *p = new QProcess(this);
#ifdef Q_OS_WIN
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    p->setProgram(program);
    p->setWorkingDirectory(QFileInfo(program).absolutePath());
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("NEEDLE_TELEMETRY"), QStringLiteral("0"));
    env.insert(QStringLiteral("DO_NOT_TRACK"), QStringLiteral("1"));
    env.insert(QStringLiteral("HF_HUB_OFFLINE"), QStringLiteral("1"));
#ifdef Q_OS_UNIX
    const QString oldPath = env.value(QStringLiteral("LD_LIBRARY_PATH"));
    env.insert(QStringLiteral("LD_LIBRARY_PATH"), QFileInfo(program).absolutePath()
               + (oldPath.isEmpty() ? QString() : QStringLiteral(":") + oldPath));
#endif
    p->setProcessEnvironment(env);
    const auto temp = m_temp;
    // Retain a task's directory until its worker is deleted, including cancellation.
    connect(p, &QObject::destroyed, [temp] { Q_UNUSED(temp); });
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p] {
        if (p == m_capture) {
            consumeRecordingOutput();
            return;
        }
        QByteArray bytes = p->property("stdout").toByteArray();
        bytes.append(p->readAllStandardOutput());
        if (bytes.size() > OutputLimit) {
            p->setProperty("outputLimit", true);
            p->kill();
            bytes.truncate(OutputLimit);
        }
        p->setProperty("stdout", bytes);
    });
    connect(p, &QProcess::readyReadStandardError, p, [p] {
        QByteArray bytes = p->property("stderr").toByteArray();
        bytes.append(p->readAllStandardError());
        // Diagnostic output is bounded; progress lines need not be retained.
        if (bytes.size() > 64 * 1024) bytes = bytes.right(64 * 1024);
        p->setProperty("stderr", bytes);
    });
    auto *watchdog = new QTimer(p);
    watchdog->setSingleShot(true);
    connect(p, &QProcess::started, watchdog, [watchdog, timeoutMs] { watchdog->start(timeoutMs); });
    connect(p, &QProcess::finished, watchdog, &QTimer::stop);
    connect(watchdog, &QTimer::timeout, p, [p] {
        p->setProperty("timedOut", true);
        p->kill();
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        const QString message = QStringLiteral("The bundled %1 process could not start: %2")
            .arg(QFileInfo(p->program()).fileName(), p->errorString());
        if (p == m_devices) {
            m_devices = nullptr;
            p->deleteLater();
            emit devicesError(message);
            emit devicesReady(QJsonArray());
        } else if (p == m_capture || p == m_converter || p == m_inference) {
            fail(message);
        }
    });
    return p;
}

void SttService::requestDevices() {
    stopProcess(m_devices);
    const QString helper = enginePath(QStringLiteral("whistle-audio"));
    if (!QFileInfo(helper).isExecutable()) {
        emit devicesError(QStringLiteral("The bundled audio helper is missing. Extract the complete application package."));
        emit devicesReady(QJsonArray());
        return;
    }
    QProcess *p = m_devices = makeProcess(helper, 20000);
    p->setArguments({QStringLiteral("devices")});
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus exit) {
        if (p != m_devices) return;
        m_devices = nullptr;
        const QByteArray output = p->property("stdout").toByteArray();
        QJsonParseError parseError;
        const auto doc = QJsonDocument::fromJson(output, &parseError);
        if (exit != QProcess::NormalExit || code != 0 || !doc.isObject()
            || !doc.object().value(QStringLiteral("devices")).isArray()) {
            emit devicesError(processDetail(p, helperFailure(output, p->property("stderr").toByteArray(),
                        QStringLiteral("Microphone discovery failed. Check audio permissions and the audio service."))));
            emit devicesReady(QJsonArray());
        } else {
            emit devicesReady(doc.object().value(QStringLiteral("devices")).toArray());
        }
        p->deleteLater();
    });
    p->start();
}

void SttService::transcribeFile(const QString &path) {
    if (!beginTask()) return;
    const QFileInfo input(path);
    if (!input.isFile() || !input.isReadable()) {
        fail(QStringLiteral("The selected audio file is missing or cannot be read."));
        return;
    }
    if (input.size() > 1024LL * 1024 * 1024) {
        fail(QStringLiteral("Choose an audio file smaller than 1 GiB and no longer than one hour."));
        return;
    }
    const quint64 generation = m_generation;
    const QString outputPath = m_temp->filePath(QStringLiteral("import.wav"));
    QProcess *p = m_converter = makeProcess(enginePath(QStringLiteral("whistle-audio")), 300000);
    p->setArguments({QStringLiteral("convert"), QStringLiteral("--input"), input.absoluteFilePath(),
                     QStringLiteral("--output"), outputPath});
    emit status(QStringLiteral("Preparing audio…"));
    connect(p, &QProcess::finished, this, [this, p, generation, outputPath](int code, QProcess::ExitStatus exit) {
        if (generation != m_generation || p != m_converter) return;
        m_converter = nullptr;
        if (code != 0 || exit != QProcess::NormalExit || QFileInfo(outputPath).size() <= 44) {
            const auto message = processDetail(p, helperFailure(p->property("stdout").toByteArray(),
                p->property("stderr").toByteArray(), QStringLiteral("The audio file could not be decoded. Supported formats are WAV, MP3 and FLAC, up to one hour.")));
            p->deleteLater();
            fail(message);
            return;
        }
        p->deleteLater();
        startInference(outputPath, true);
    });
    p->start();
}

void SttService::startRecording(const QString &deviceId, bool live) {
    if (!beginTask()) return;
    m_live = live;
    m_recording = true;
    m_recordingPath = m_temp->filePath(QStringLiteral("recording.wav"));
    const quint64 generation = m_generation;
    QProcess *p = m_capture = makeProcess(enginePath(QStringLiteral("whistle-audio")), 3630000);
    QStringList args{QStringLiteral("record"), QStringLiteral("--output"), m_recordingPath,
                     QStringLiteral("--max-seconds"), QStringLiteral("3600")};
    if (!deviceId.isEmpty()) args << QStringLiteral("--device") << deviceId;
    p->setArguments(args);
    emit status(QStringLiteral("Opening microphone…"));
    connect(p, &QProcess::finished, this, [this, p, generation](int code, QProcess::ExitStatus exit) {
        if (generation != m_generation || p != m_capture) return;
        consumeRecordingOutput();
        m_capture = nullptr;
        m_recording = false;
        m_liveTimer.stop();
        emit recordingChanged(false);
        stopProcess(m_inference);
        if (code != 0 || exit != QProcess::NormalExit || !m_captureFailure.isEmpty()) {
            const QString fallback = m_captureFailure.isEmpty()
                ? QStringLiteral("The microphone recording failed. Check the selected device and microphone permissions.")
                : m_captureFailure;
            const auto message = processDetail(p, helperFailure({}, p->property("stderr").toByteArray(), fallback));
            p->deleteLater();
            fail(message);
            return;
        }
        p->deleteLater();
        if (QFileInfo(m_recordingPath).size() < 44 + 1600) {
            fail(QStringLiteral("The recording is too short. Record a little longer and try again."));
            return;
        }
        emit status(QStringLiteral("Finalizing transcript…"));
        startInference(m_recordingPath, true);
    });
    p->start();
    // Opening a driver can stall before it reports that recording has begun.
    QTimer::singleShot(20000, p, [this, p, generation] {
        if (generation == m_generation && p == m_capture && !m_captureStarted) {
            m_captureFailure = QStringLiteral("The microphone did not start. Check its connection and microphone permissions.");
            p->kill();
        }
    });
}

void SttService::consumeRecordingOutput() {
    if (!m_capture) return;
    m_recordEvents.append(m_capture->readAllStandardOutput());
    if (m_recordEvents.size() > 128 * 1024) {
        m_captureFailure = QStringLiteral("The microphone returned an invalid event stream.");
        m_capture->kill();
        m_recordEvents.clear();
        return;
    }
    while (true) {
        const qsizetype end = m_recordEvents.indexOf('\n');
        if (end < 0) break;
        const QByteArray line = m_recordEvents.left(end).trimmed();
        m_recordEvents.remove(0, end + 1);
        if (line.isEmpty()) continue;
        const auto doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            m_captureFailure = QStringLiteral("The microphone returned an unreadable event.");
            m_capture->kill();
            return;
        }
        const auto event = doc.object();
        const auto type = event.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("started")) {
            m_captureStarted = true;
            emit recordingChanged(true);
            emit status(m_live ? QStringLiteral("Listening · live text updates as recognition finishes…")
                               : QStringLiteral("Recording · press Stop to transcribe."));
            if (m_live && !m_stopRequested) m_liveTimer.start();
        } else if (type == QStringLiteral("level")) {
            const double seconds = event.value(QStringLiteral("seconds")).toDouble();
            const double peak = event.value(QStringLiteral("peak")).toDouble();
            if (std::isfinite(seconds) && std::isfinite(peak))
                emit levelChanged(std::clamp(peak, 0.0, 1.0), std::clamp(seconds, 0.0, 3600.0));
        } else if (type == QStringLiteral("error")) {
            m_captureFailure = event.value(QStringLiteral("message")).toString(
                QStringLiteral("The microphone reported an error."));
        }
    }
}

void SttService::stopRecording() {
    if (!m_capture || m_stopRequested) return;
    m_stopRequested = true;
    m_liveTimer.stop();
    stopProcess(m_inference);
    emit status(QStringLiteral("Stopping microphone…"));
    m_capture->write("stop\n");
    m_capture->closeWriteChannel();
    const quint64 generation = m_generation;
    QProcess *p = m_capture;
    QTimer::singleShot(10000, p, [this, p, generation] {
        if (generation == m_generation && p == m_capture) {
            m_captureFailure = QStringLiteral("The microphone did not stop cleanly. Try the recording again.");
            p->kill();
        }
    });
}

bool SttService::writeSnapshot(qint64 firstFrame, qint64 lastFrame, const QString &target) {
    if (firstFrame < 0 || lastFrame <= firstFrame || lastFrame - firstFrame > LiveWindowFrames)
        return false;
    QFile input(m_recordingPath);
    if (!input.open(QIODevice::ReadOnly)) return false;
    const QByteArray header = input.read(44);
    if (header.size() != 44 || header.left(4) != "RIFF" || header.mid(8, 8) != "WAVEfmt "
        || qFromLittleEndian<quint32>(header.constData() + 24) != 16000
        || qFromLittleEndian<quint16>(header.constData() + 22) != 1
        || qFromLittleEndian<quint16>(header.constData() + 34) != 16)
        return false;
    if (!input.seek(44 + firstFrame * BytesPerFrame)) return false;
    const QByteArray pcm = input.read((lastFrame - firstFrame) * BytesPerFrame);
    if (pcm.size() != (lastFrame - firstFrame) * BytesPerFrame) return false;
    QFile output(target);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray newHeader = wavHeader(quint32(pcm.size()));
    if (output.write(newHeader) != newHeader.size() || output.write(pcm) != pcm.size()) return false;
    return output.flush();
}

void SttService::updateLive() {
    if (!m_busy || !m_live || !m_captureStarted || !m_recording || m_stopRequested || m_inference)
        return;
    const qint64 availableFrames = std::min(MaxFrames,
        std::max(qint64(0), QFileInfo(m_recordingPath).size() - 44) / BytesPerFrame);
    const qint64 end = std::min(availableFrames, m_committedFrame + LiveWindowFrames);
    if (end - m_committedFrame < SampleRate * 2 || end - m_lastLiveEnd < SampleRate * 2)
        return;
    const QString path = m_temp->filePath(QStringLiteral("live.wav"));
    if (!writeSnapshot(m_committedFrame, end, path)) return;
    m_lastLiveEnd = end;
    startInference(path, false, m_committedFrame, end);
}

void SttService::startInference(const QString &wavPath, bool final, qint64 startFrame, qint64 endFrame) {
    if (!m_busy || m_inference) return;
    const quint64 generation = m_generation;
    const bool whistle = m_jobModel == QStringLiteral("whistle");
    const QString outputBase = m_temp->filePath(final ? QStringLiteral("final") : QStringLiteral("live"));
    const QString jsonPath = outputBase + QStringLiteral(".json");
    QFile::remove(jsonPath);
    QProcess *p = m_inference = makeProcess(enginePath(whistle ? QStringLiteral("needle") : QStringLiteral("whisper-cli")),
                                           final ? 1800000 : 120000);
    QStringList args;
    if (whistle) {
        args = {QStringLiteral("--model"), modelPath(), QStringLiteral("--audio"), wavPath,
                QStringLiteral("--audio-depth"), QStringLiteral("8"),
                QStringLiteral("--threads"), QString::number(m_jobThreads)};
        if (m_jobLanguage != QStringLiteral("auto"))
            args << QStringLiteral("--audio-language") << m_jobLanguage;
    } else {
        args = {QStringLiteral("-m"), modelPath(), QStringLiteral("-f"), wavPath,
                QStringLiteral("-l"), m_jobLanguage, QStringLiteral("-oj"),
                QStringLiteral("-of"), outputBase, QStringLiteral("-t"),
                QString::number(m_jobThreads), QStringLiteral("--no-gpu")};
    }
    p->setArguments(args);
    if (final) emit status(QStringLiteral("Transcribing with %1…")
        .arg(whistle ? QStringLiteral("Whistle") : QStringLiteral("Whisper ") + m_jobModel));
    connect(p, &QProcess::finished, this,
            [this, p, generation, final, jsonPath, startFrame, endFrame](int code, QProcess::ExitStatus exit) {
        if (generation != m_generation || p != m_inference) return;
        m_inference = nullptr;
        const QByteArray output = p->property("stdout").toByteArray();
        if (code != 0 || exit != QProcess::NormalExit) {
            const auto message = processDetail(p, helperFailure(output, p->property("stderr").toByteArray(),
                QStringLiteral("Recognition failed. Try another dictation model.")));
            p->deleteLater();
            if (final) fail(message);
            else {
                m_live = false;
                m_liveTimer.stop();
                emit status(QStringLiteral("Live recognition is unavailable; recording continues. Press Stop for final transcription."));
            }
            return;
        }
        QString parseError;
        const QString text = parseTranscript(output, jsonPath, &parseError);
        p->deleteLater();
        if (!parseError.isEmpty()) {
            if (final) fail(parseError);
            else {
                m_live = false;
                m_liveTimer.stop();
                emit status(QStringLiteral("Live recognition returned unreadable text; press Stop for final transcription."));
            }
            return;
        }
        if (final) {
            emit transcript(text, true);
            emit status(text.isEmpty() ? QStringLiteral("No speech was recognized.") : QStringLiteral("Transcription complete."));
            finishTask();
        } else if (m_recording && !m_stopRequested) {
            const QString combined = appendWithoutOverlap(m_committedText, text);
            emit transcript(combined, false);
            if (endFrame - startFrame >= CommitWindowFrames) {
                m_committedText = combined;
                m_committedFrame = endFrame - OverlapFrames;
            }
        }
    });
    p->start();
}

QString SttService::parseTranscript(const QByteArray &output, const QString &jsonPath, QString *parseError) const {
    QByteArray json;
    if (m_jobModel == QStringLiteral("whistle")) {
        const qsizetype first = output.indexOf('{');
        const qsizetype last = output.lastIndexOf('}');
        if (first >= 0 && last >= first) json = output.mid(first, last - first + 1);
    } else {
        QFile file(jsonPath);
        if (file.size() <= OutputLimit && file.open(QIODevice::ReadOnly)) json = file.readAll();
    }
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(json, &error);
    if (!doc.isObject()) {
        *parseError = QStringLiteral("The recognition engine returned an unreadable transcript.");
        return {};
    }
    if (m_jobModel == QStringLiteral("whistle")) {
        const auto value = doc.object().value(QStringLiteral("text"));
        if (!value.isString()) {
            *parseError = QStringLiteral("Whistle did not return a text transcript.");
            return {};
        }
        return value.toString().trimmed();
    }
    const auto array = doc.object().value(QStringLiteral("transcription"));
    if (!array.isArray()) {
        *parseError = QStringLiteral("Whisper did not return a text transcript.");
        return {};
    }
    QStringList parts;
    for (const auto &segment : array.toArray()) {
        const QString text = segment.toObject().value(QStringLiteral("text")).toString().trimmed();
        if (!text.isEmpty()) parts << text;
    }
    return parts.join(QLatin1Char(' '));
}

QString SttService::appendWithoutOverlap(const QString &before, const QString &after) {
    if (before.isEmpty()) return after;
    if (after.isEmpty()) return before;
    const QRegularExpression whitespace(QStringLiteral("\\s+"));
    const QStringList oldWords = before.split(whitespace, Qt::SkipEmptyParts);
    const QStringList newWords = after.split(whitespace, Qt::SkipEmptyParts);
    const auto normalized = [](QString word) {
        word.remove(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")));
        return word.toCaseFolded();
    };
    const int maxMatch = std::min({12, int(oldWords.size()), int(newWords.size())});
    int overlap = 0;
    for (int count = maxMatch; count >= 1; --count) {
        bool equal = true;
        for (int i = 0; i < count; ++i) {
            const QString word = normalized(oldWords.at(oldWords.size() - count + i));
            if (word.isEmpty() || word != normalized(newWords.at(i))) { equal = false; break; }
        }
        if (equal) { overlap = count; break; }
    }
    const QString tail = newWords.mid(overlap).join(QLatin1Char(' '));
    return tail.isEmpty() ? before : before + QLatin1Char(' ') + tail;
}

QString SttService::helperFailure(const QByteArray &output, const QByteArray &errors, const QString &fallback) {
    for (const QByteArray &line : output.split('\n')) {
        const auto event = QJsonDocument::fromJson(line).object();
        if (event.value(QStringLiteral("type")).toString() == QStringLiteral("error")) {
            const QString message = event.value(QStringLiteral("message")).toString();
            if (!message.isEmpty()) return message.left(1200);
        }
    }
    // Keep native diagnostics local and bounded; no input audio or transcript is
    // deliberately included in the error. Often a helper supplies better wording.
    const QString detail = QString::fromUtf8(errors).trimmed();
    return detail.isEmpty() ? fallback : fallback + QStringLiteral("\n") + detail.right(1000);
}

void SttService::stopProcess(QProcess *&process) {
    if (!process) return;
    QProcess *p = process;
    process = nullptr;
    disconnect(p, nullptr, this, nullptr);
    if (p->state() == QProcess::NotRunning) p->deleteLater();
    else {
        connect(p, &QProcess::finished, p, &QObject::deleteLater);
        p->kill();
    }
}

void SttService::resetTask() {
    ++m_generation;
    m_liveTimer.stop();
    const bool recording = m_recording;
    m_recording = false;
    m_busy = false;
    m_live = false;
    m_stopRequested = false;
    stopProcess(m_capture);
    stopProcess(m_converter);
    stopProcess(m_inference);
    m_temp.reset();
    m_recordEvents.clear();
    if (recording) emit recordingChanged(false);
}

void SttService::fail(const QString &message) {
    resetTask();
    emit status(QStringLiteral("Transcription failed."));
    emit error(message);
    emit finished();
}

void SttService::finishTask() {
    resetTask();
    emit finished();
}

void SttService::cancel() {
    const bool wasBusy = m_busy;
    resetTask();
    if (wasBusy) {
        emit status(QStringLiteral("Transcription cancelled."));
        emit finished();
    }
}

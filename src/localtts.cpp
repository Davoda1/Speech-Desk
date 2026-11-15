#include "localtts.h"
#include "builtinttscatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

namespace {
constexpr qsizetype MaxAudioBytes = 64 * 1024 * 1024;
constexpr qsizetype MaxLogBytes = 2 * 1024 * 1024;

bool regularFile(const QString &path)
{
    const QFileInfo info(path);
    return info.isFile() && info.size() > 0;
}

QProcessEnvironment workerEnvironment(const QString &assets, const QString &work, int threads)
{
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto &key : env.keys()) {
        const auto upper = key.toUpper();
        for (const auto &prefix : {"PYTHON", "HF_", "TRANSFORMERS_", "ORT_", "ONNX_", "CUDA_", "ROCM_", "PHONEMIZER_"}) {
            if (upper.startsWith(QLatin1String(prefix))) { env.remove(key); break; }
        }
    }
    env.insert("PYTHONPATH", assets + "/runtime/packages");
    env.insert("PYTHONNOUSERSITE", "1");
    env.insert("PYTHONDONTWRITEBYTECODE", "1");
    env.insert("PYTHONUTF8", "1");
    env.insert("PYTHONIOENCODING", "utf-8");
    env.insert("HF_HOME", work + "/cache/huggingface");
    env.insert("HF_HUB_OFFLINE", "1");
    env.insert("HF_HUB_DISABLE_TELEMETRY", "1");
    env.insert("ORT_DISABLE_TELEMETRY", "1");
    env.insert("DO_NOT_TRACK", "1");
    env.insert("TRANSFORMERS_OFFLINE", "1");
    env.insert("CUDA_VISIBLE_DEVICES", "");
    env.insert("ONNX_PROVIDER", "CPUExecutionProvider");
    for (const auto &key : {"OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "RAYON_NUM_THREADS"})
        env.insert(QLatin1String(key), QString::number(threads));
    env.insert("XDG_CACHE_HOME", work + "/cache");
    for (const auto &key : {"TMPDIR", "TMP", "TEMP"}) env.insert(QLatin1String(key), work);
#ifdef Q_OS_WIN
    const auto systemRoot = env.value("SystemRoot", "C:/Windows");
    env.insert("PATH", QDir::toNativeSeparators(assets + "/runtime/python;" + assets + "/engines/pocket;" + systemRoot + "/System32;" + systemRoot));
#else
    env.insert("LD_LIBRARY_PATH", assets + "/runtime/python/lib");
    env.insert("PATH", assets + "/runtime/python/bin:/usr/bin:/bin");
#endif
    return env;
}
}

LocalTts::LocalTts(QObject *parent) : QObject(parent), m_timeout(new QTimer(this))
{
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        fail(tr("Offline speech generation timed out. Try a shorter passage."));
    });
}

LocalTts::~LocalTts() { cancel(); }

QString LocalTts::assetRoot()
{
    const auto app = QCoreApplication::applicationDirPath();
    for (const auto &root : {app + "/resources/tts-assets", app + "/../lib/speechdesk/resources/tts-assets"}) {
        if (QFileInfo::exists(root + "/voice_light.py")) return QDir(root).absolutePath();
    }
    return QDir(app + "/resources/tts-assets").absolutePath();
}

void LocalTts::cancel()
{
    m_timeout->stop();
    auto *process = m_process;
    m_process = nullptr;
    if (process) {
        process->disconnect(this);
        const auto pid = process->processId();
        if (process->state() != QProcess::NotRunning) {
#ifdef Q_OS_WIN
            // Match the Reader adapter: terminate Python and its native Pocket
            // descendants together before removing their temporary files.
            if (pid > 0) {
                QProcess killer;
                const auto root = qEnvironmentVariable("SystemRoot", "C:/Windows");
                killer.start(root + "/System32/taskkill.exe", {"/PID", QString::number(pid), "/T", "/F"});
                killer.waitForFinished(3000);
                if (killer.state() != QProcess::NotRunning) { killer.kill(); killer.waitForFinished(1000); }
            }
#else
            // setpgid in the child keeps its native descendants in this group.
            if (pid > 0) ::kill(-static_cast<pid_t>(pid), SIGKILL);
#endif
            process->kill();
            process->waitForFinished(1000);
        }
        process->deleteLater();
    }
    m_work.reset();
    m_stdout.clear();
    m_logBytes = 0;
    m_completed = false;
    m_output.clear();
}

void LocalTts::fail(const QString &message)
{
    cancel();
    emit error(message);
}

void LocalTts::consumeOutput()
{
    if (!m_process) return;
    const auto chunk = m_process->readAllStandardOutput();
    m_logBytes += chunk.size();
    if (m_logBytes > MaxLogBytes) { fail(tr("The offline speech runtime exceeded its diagnostic output limit.")); return; }
    m_stdout += chunk;
    qsizetype newline;
    while ((newline = m_stdout.indexOf('\n')) >= 0) {
        const auto line = m_stdout.left(newline);
        m_stdout.remove(0, newline + 1);
        const auto event = QJsonDocument::fromJson(line).object();
        if (event.value("type").toString() == "result" && event.value("ok").toBool()) {
            m_completed = QFileInfo(event.value("output_path").toString()).canonicalFilePath() == QFileInfo(m_output).canonicalFilePath()
                && regularFile(m_output);
        }
    }
}

void LocalTts::synthesize(const QJsonObject &profile, const QString &text)
{
    cancel();
    const auto engine = profile.value("model").toString("pocket");
    const auto voice = profile.value("voice").toString(engine == "kokoro" ? "af_heart" : "alba");
    if (engine != "pocket" && engine != "kokoro") { emit error(tr("Choose Pocket TTS or Kokoro for offline speech.")); return; }
    const auto catalog = builtinTtsCapabilities(engine);
    bool knownVoice = false;
    for (const auto &item : catalog.value("voices").toArray())
        if (item.toObject().value("id").toString() == voice) knownVoice = true;
    if (!knownVoice) { emit error(tr("Choose a bundled voice for the selected offline model.")); return; }
    if (text.trimmed().isEmpty() || text.size() > 100000 || text.toUtf8().size() > 256 * 1024) {
        emit error(tr("The offline speech text is empty or too long.")); return;
    }
    const auto assets = assetRoot();
#ifdef Q_OS_WIN
    const auto python = assets + "/runtime/python/python.exe";
    const auto pocket = assets + "/engines/pocket/pocket-tts.exe";
#else
    const auto python = assets + "/runtime/python/bin/python3";
    const auto pocket = assets + "/engines/pocket/pocket-tts";
#endif
    const auto script = assets + "/voice_light.py";
    QStringList required{python, script};
    if (engine == "pocket") {
        required << pocket << assets + "/models/pocket/model.safetensors"
                 << assets + "/models/pocket/tokenizer.json" << assets + "/models/pocket/english_2026-10.upstream.yaml"
                 << assets + "/models/pocket/voices/" + voice + ".safetensors";
    } else {
        required << assets + "/models/kokoro/kokoro-v1.0.onnx" << assets + "/models/kokoro/voices-v1.0.bin";
    }
    for (const auto &path : required) if (!regularFile(path)) {
        emit error(tr("The bundled offline speech files are missing or incomplete. Re-extract the complete SpeechDesk package.")); return;
    }
    m_work = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/speechdesk-tts-XXXXXX");
    if (!m_work->isValid()) { fail(tr("Could not create a temporary offline speech folder.")); return; }
    const auto work = QFileInfo(m_work->path()).canonicalFilePath();
    m_output = work + "/speech.wav";
    const int threads = qBound(1, QThread::idealThreadCount(), 4);
    const QJsonObject request{{"engine", engine}, {"voice", voice}, {"text", text}, {"threads", threads}, {"stream", false},
        {"output_path", m_output}, {"model_root", assets + "/models"}, {"engine_root", assets + "/engines"}, {"cache_root", work + "/cache"}};
    auto *process = new QProcess(this);
    m_process = process;
    process->setWorkingDirectory(work);
    process->setProcessEnvironment(workerEnvironment(assets, work, threads));
#ifdef Q_OS_UNIX
    process->setChildProcessModifier([] { ::setpgid(0, 0); });
#endif
    connect(process, &QProcess::started, this, [this, process, request] {
        if (m_process != process) return;
        process->write(QJsonDocument(request).toJson(QJsonDocument::Compact));
        process->closeWriteChannel();
    });
    connect(process, &QProcess::readyReadStandardOutput, this, &LocalTts::consumeOutput);
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        if (m_process != process) return;
        m_logBytes += process->readAllStandardError().size();
        if (m_logBytes > MaxLogBytes) fail(tr("The offline speech runtime exceeded its diagnostic output limit."));
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (m_process != process) return;
        if (error == QProcess::FailedToStart) fail(tr("The bundled speech runtime could not start. Re-extract the complete application package."));
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process](int code, QProcess::ExitStatus exitStatus) {
        if (m_process != process) return;
        consumeOutput();
        if (m_process != process) return;
        if (code != 0 || exitStatus != QProcess::NormalExit || !m_completed) {
            fail(tr("The bundled speech engine could not complete audio generation. Check the selected voice and try a shorter passage.")); return;
        }
        QFile output(m_output);
        if (!output.open(QIODevice::ReadOnly) || output.size() <= 44 || output.size() > MaxAudioBytes) {
            fail(tr("The offline speech engine returned invalid or oversized audio.")); return;
        }
        auto audio = output.readAll();
        output.close();
        if (audio.left(4) != "RIFF" || audio.mid(8, 4) != "WAVE") {
            fail(tr("The offline speech engine returned invalid WAV audio.")); return;
        }
        cancel();
        emit audioReady(std::move(audio));
    });
    emit status(tr("Generating offline speech on CPU…"));
    m_timeout->start(qBound(10, profile.value("timeoutSeconds").toInt(180), 1800) * 1000);
    process->start(python, {"-s", "-B", script});
}

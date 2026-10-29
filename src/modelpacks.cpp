#include "modelpacks.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <vector>

namespace {
constexpr qint64 MaxEntry = 2LL * 1024 * 1024 * 1024;
constexpr qint64 ChunkSize = 1024 * 1024;

// A bounded random-access view over one ZIP or its numbered delivery parts.
// Qt's parser below still sees exactly the original archive bytes. Parts are
// never joined into a temporary multi-gigabyte file and no extractor is used.
class ArchiveInput final : public QIODevice {
public:
    bool openArchive(const QString &path) {
        QStringList paths;
        const QFileInfo selected(path);
        const QString name = selected.fileName();
        const QRegularExpression numbered(QStringLiteral("\\.zip\\.([0-9]{3})$"), QRegularExpression::CaseInsensitiveOption);
        const auto match = numbered.match(name);
        if (match.hasMatch()) {
            if (match.captured(1) != "001") {
                setErrorString("Select the first model-pack part (.zip.001).");
                return false;
            }
            const QString prefix = name.left(name.size() - 3);
            QMap<int, QString> parts;
            for (const QFileInfo &file : QDir(selected.absolutePath()).entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
#ifdef Q_OS_WIN
                const bool samePrefix = file.fileName().startsWith(prefix, Qt::CaseInsensitive);
#else
                const bool samePrefix = file.fileName().startsWith(prefix);
#endif
                if (!samePrefix) continue;
                const QString suffix = file.fileName().mid(prefix.size());
                if (!QRegularExpression(QStringLiteral("^[0-9]{3}$")).match(suffix).hasMatch()) continue;
                const int number = suffix.toInt();
                if (number < 1 || number > 4 || parts.contains(number)) {
                    setErrorString("A model pack supports at most four consecutively numbered parts (.001 to .004).");
                    return false;
                }
                parts.insert(number, file.absoluteFilePath());
            }
            const int last = parts.isEmpty() ? 2 : qMax(2, parts.lastKey());
            for (int number = 1; number <= last; ++number) {
                if (!parts.contains(number)) {
                    setErrorString(QString("Missing model-pack part %1%2. Put every numbered part in the same folder, then select .zip.001.")
                        .arg(prefix, QString::number(number).rightJustified(3, '0')));
                    return false;
                }
                paths.append(parts[number]);
            }
            m_multipart = true;
        } else {
            paths.append(selected.absoluteFilePath());
        }
        for (const QString &partPath : paths) {
            auto file = std::make_unique<QFile>(partPath);
            if (!QFileInfo(partPath).isFile() || !file->open(QIODevice::ReadOnly)) {
                setErrorString("Cannot read model-pack file " + QFileInfo(partPath).fileName() + ": " + file->errorString());
                return false;
            }
            if (file->size() <= 0 || file->size() > MaxEntry + 1024 * 1024 - m_size) {
                setErrorString("The model pack is empty or exceeds the supported total size.");
                return false;
            }
            m_offsets.append(m_size);
            m_size += file->size();
            m_files.push_back(std::move(file));
        }
        return QIODevice::open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    qint64 size() const override { return m_size; }
    qint64 pos() const override { return m_position; }
    bool seek(qint64 position) override {
        if (position < 0 || position > m_size || !QIODevice::seek(position)) return false;
        m_position = position;
        return true;
    }
    bool multipart() const { return m_multipart; }
protected:
    qint64 readData(char *data, qint64 maximum) override {
        if (maximum <= 0 || m_position == m_size) return 0;
        qint64 received = 0;
        while (received < maximum && m_position < m_size) {
            const int index = int(std::upper_bound(m_offsets.begin(), m_offsets.end(), m_position) - m_offsets.begin()) - 1;
            QFile &file = *m_files[size_t(index)];
            const qint64 offset = m_position - m_offsets[index];
            const qint64 end = index + 1 < m_offsets.size() ? m_offsets[index + 1] : m_size;
            const qint64 amount = qMin(maximum - received, end - m_position);
            if (!file.seek(offset)) { setErrorString(file.errorString()); return -1; }
            const qint64 count = file.read(data + received, amount);
            if (count != amount) {
                setErrorString("A model-pack part could not be read completely. Check that all parts finished downloading.");
                return -1;
            }
            received += count;
            m_position += count;
        }
        return received;
    }
    qint64 writeData(const char *, qint64) override { return -1; }
private:
    std::vector<std::unique_ptr<QFile>> m_files;
    QVector<qint64> m_offsets;
    qint64 m_size = 0, m_position = 0;
    bool m_multipart = false;
};

struct Entry { QByteArray name; quint32 size, crc, offset; qint64 dataOffset; };
quint16 u16(const QByteArray &b, int n) { return qFromLittleEndian<quint16>(b.constData() + n); }
quint32 u32(const QByteArray &b, int n) { return qFromLittleEndian<quint32>(b.constData() + n); }
quint32 crcUpdate(quint32 value, const QByteArray &data) {
    static const auto table = [] {
        std::array<quint32, 256> result{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 n = i;
            for (int bit = 0; bit < 8; ++bit) n = (n >> 1) ^ ((n & 1) ? 0xedb88320U : 0);
            result[i] = n;
        }
        return result;
    }();
    for (unsigned char byte : data) value = (value >> 8) ^ table[(value ^ byte) & 255];
    return value;
}
QJsonObject identity(const ModelPackSpec &s) {
    return {{"format", "speechdesk-model-pack"}, {"version", 1}, {"id", s.id},
            {"filename", s.fileName}, {"size", s.size}, {"sha256", s.sha256}};
}
bool matches(const QJsonObject &o, const ModelPackSpec &s) {
    return o.value("format").toString() == "speechdesk-model-pack"
        && o.value("version").toDouble() == 1 && o.value("id").toString() == s.id
        && o.value("filename").toString() == s.fileName
        && o.value("size").toDouble() == double(s.size)
        && o.value("sha256").toString() == s.sha256;
}
bool installed(const QString &directory, const ModelPackSpec &spec) {
    const QFileInfo weights(QDir(directory).filePath(spec.fileName));
    if (!weights.isFile() || weights.isSymLink() || !weights.isReadable() || weights.size() != spec.size)
        return false;
    const QString metadataPath = QDir(directory).filePath(spec.id + ".json");
    if (QFileInfo(metadataPath).isSymLink()) return false;
    QFile metadata(metadataPath);
    if (metadata.size() > 16384 || !metadata.open(QIODevice::ReadOnly)) return false;
    return matches(QJsonDocument::fromJson(metadata.readAll()).object(), spec);
}
}

ModelPacks::ModelPacks(QObject *parent) : QObject(parent) {}
ModelPacks::~ModelPacks() {
    cancel();
    if (m_thread) {
        disconnect(m_thread, nullptr, this, nullptr);
        m_thread->wait();
        delete m_thread;
    }
}

QVector<ModelPackSpec> ModelPacks::catalog() {
    return {{"large-v3-turbo", "Whisper Large v3 Turbo · Q5_0", "ggml-large-v3-turbo-q5_0.bin",
             "394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2", 574041195},
            {"large-v3", "Whisper Large v3 · Q5_0", "ggml-large-v3-q5_0.bin",
             "d75795ecff3f83b5faa89d1900604ad8c780abd5739fae406de19f23ecd98ad1", 1081140203}};
}

QString ModelPacks::configDirectory() {
    const QString override = qEnvironmentVariable("SPEECHDESK_CONFIG_DIR");
    if (!override.isEmpty()) return override;
#ifdef Q_OS_WIN
    return QDir(qEnvironmentVariable("APPDATA", QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))).filePath("Speech-Desk");
#else
    return QDir(qEnvironmentVariable("XDG_CONFIG_HOME", QDir::home().filePath(".config"))).filePath("Speech-Desk");
#endif
}
QString ModelPacks::modelsDirectory() { return QDir(configDirectory()).filePath("models"); }
QVector<ModelPackSpec> ModelPacks::installedModels() {
    QVector<ModelPackSpec> result;
    for (const auto &spec : catalog()) if (installed(modelsDirectory(), spec)) result.append(spec);
    return result;
}
QString ModelPacks::resolve(const QString &id) {
    for (const auto &spec : catalog())
        if (spec.id == id && installed(modelsDirectory(), spec))
            return QDir(modelsDirectory()).absoluteFilePath(spec.fileName);
    return {};
}

void ModelPacks::cancel() { if (m_cancel) m_cancel->store(true); }
void ModelPacks::importArchive(const QString &path) {
    if (busy()) { emit error("Wait for the current model import to finish."); return; }
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancelFlag = m_cancel;
    const auto result = std::make_shared<ImportResult>();
    const QString directory = modelsDirectory();
    m_thread = QThread::create([this, path, directory, cancelFlag, result] {
        *result = importPack(path, directory, catalog(), *cancelFlag,
            [this](qint64 received, qint64 total) { emit progress(received, total); });
    });
    connect(m_thread, &QThread::finished, this, [this, result] {
        QThread *thread = m_thread;
        m_thread = nullptr;
        m_cancel.reset();
        thread->deleteLater();
        if (result->cancelled) emit cancelled();
        else if (!result->error.isEmpty()) emit error(result->error);
        else emit completed(result->id);
    });
    m_thread->start();
}

ModelPacks::ImportResult ModelPacks::importPack(const QString &archive, const QString &directory,
    const QVector<ModelPackSpec> &specs, std::atomic_bool &cancelFlag,
    const std::function<void(qint64, qint64)> &report) {
    const auto failure = [](const QString &message) { return ImportResult{{}, message, false}; };
    const auto cancelled = [] { return ImportResult{{}, {}, true}; };
    if (cancelFlag.load()) return cancelled();
    ArchiveInput input;
    if (!input.openArchive(archive)) return failure(input.errorString());
    if (input.size() < 22 || input.size() > MaxEntry + 1024 * 1024)
        return failure("The selected file is not a supported SpeechDesk model pack.");
    input.seek(qMax<qint64>(0, input.size() - 65557));
    const QByteArray tail = input.readAll();
    int end = -1;
    for (int i = tail.size() - 22; i >= 0; --i)
        if (u32(tail, i) == 0x06054b50U && i + 22 + u16(tail, i + 20) == tail.size()) { end = i; break; }
    if (end < 0) return failure(input.multipart()
        ? "The model pack is incomplete. Put every numbered part (.001, .002 and any .003/.004) in the same folder, then select .zip.001."
        : "The model pack is incomplete or is not a ZIP archive.");
    const qint64 endOffset = input.size() - tail.size() + end;
    const quint16 count = u16(tail, end + 10);
    const quint32 centralSize = u32(tail, end + 12), centralOffset = u32(tail, end + 16);
    if (u16(tail, end + 4) || u16(tail, end + 6) || count != u16(tail, end + 8)
        || count != 4 || quint64(centralOffset) + centralSize != quint64(endOffset)
        || centralSize > 4096)
        return failure("Use an original SpeechDesk pack. Split, ZIP64 or modified archives are not supported.");
    if (!input.seek(centralOffset)) return failure("Cannot read the model pack index.");
    QVector<Entry> entries;
    QSet<QByteArray> names;
    for (int i = 0; i < count; ++i) {
        const QByteArray header = input.read(46);
        if (header.size() != 46 || u32(header, 0) != 0x02014b50U)
            return failure("The model pack index is damaged.");
        const quint16 flags = u16(header, 8), nameLength = u16(header, 28);
        const quint32 attributes = u32(header, 38), size = u32(header, 24);
        const quint32 fileType = (attributes >> 16) & 0170000;
        if (u16(header, 6) > 20 || (flags & ~0x0800) || u16(header, 10) != 0
            || u32(header, 20) != size || size > MaxEntry || !nameLength || nameLength > 128
            || u16(header, 30) || u16(header, 32) || u16(header, 34)
            || (attributes & 0x10) || (fileType != 0 && fileType != 0100000))
            return failure("Use the unmodified SpeechDesk ZIP pack. Compressed, encrypted, linked or ZIP64 entries are not supported.");
        const QByteArray name = input.read(nameLength);
        if (name.size() != nameLength || names.contains(name) || name.contains('/')
            || name.contains('\\') || name.contains(':') || name.contains('\0') || name.contains(".."))
            return failure("The model pack contains an invalid or repeated file path.");
        names.insert(name);
        entries.append({name, size, u32(header, 16), u32(header, 42), 0});
    }
    if (input.pos() != qint64(centralOffset) + centralSize)
        return failure("The model pack index has unexpected content.");
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.offset < b.offset; });
    qint64 previousEnd = 0;
    for (Entry &entry : entries) {
        if (entry.offset != previousEnd || !input.seek(entry.offset))
            return failure("The model pack has overlapping or unexpected file records.");
        const QByteArray header = input.read(30);
        if (header.size() != 30 || u32(header, 0) != 0x04034b50U || u16(header, 4) > 20
            || (u16(header, 6) & ~0x0800) || u16(header, 8) != 0
            || u32(header, 14) != entry.crc || u32(header, 18) != entry.size
            || u32(header, 22) != entry.size || u16(header, 26) != entry.name.size()
            || u16(header, 28) != 0 || input.read(entry.name.size()) != entry.name)
            return failure("The model pack file headers do not match its index.");
        entry.dataOffset = input.pos();
        previousEnd = entry.dataOffset + entry.size;
        if (previousEnd > centralOffset) return failure("The model pack contains a truncated file.");
    }
    if (previousEnd != centralOffset) return failure("The model pack has unexpected trailing file content.");
    const auto find = [&entries](const QByteArray &name) -> const Entry * {
        for (const auto &entry : entries) if (entry.name == name) return &entry;
        return nullptr;
    };
    const Entry *manifest = find("manifest.json");
    if (!manifest || manifest->size > 16384) return failure("The model pack manifest is missing or too large.");
    input.seek(manifest->dataOffset);
    const QByteArray manifestBytes = input.read(manifest->size);
    if (manifestBytes.size() != manifest->size || (crcUpdate(0xffffffffU, manifestBytes) ^ 0xffffffffU) != manifest->crc)
        return failure("The model pack manifest failed its integrity check.");
    const QJsonObject metadata = QJsonDocument::fromJson(manifestBytes).object();
    ModelPackSpec spec;
    for (const auto &candidate : specs) if (matches(metadata, candidate)) { spec = candidate; break; }
    if (spec.id.isEmpty()) return failure("This model pack is not supported by this version of SpeechDesk.");
    const Entry *weights = find(spec.fileName.toUtf8()), *license = find("LICENSE.txt"), *readme = find("README.txt");
    if (!weights || weights->size != spec.size || !license || !readme
        || license->size > 131072 || readme->size > 131072)
        return failure("The model pack does not contain the expected weights and notices.");
    if (cancelFlag.load()) return cancelled();
    if (!QDir().mkpath(directory)) return failure("Cannot create the dictation models directory.");
    const auto target = [&directory](const QString &name) { return QDir(directory).filePath(name); };
    for (const QString &name : {spec.fileName, spec.id + ".json", spec.id + ".LICENSE.txt", spec.id + ".README.txt"})
        if (QFileInfo(target(name)).isSymLink()) return failure("A model destination is a link. Choose a regular settings directory.");
    QSaveFile model(target(spec.fileName));
    model.setDirectWriteFallback(false);
    if (!model.open(QIODevice::WriteOnly)) return failure("Cannot create the model file: " + model.errorString());
    QCryptographicHash sha(QCryptographicHash::Sha256);
    quint32 crc = 0xffffffffU;
    qint64 written = 0;
    input.seek(weights->dataOffset);
    report(0, spec.size);
    while (written < spec.size) {
        if (cancelFlag.load()) return cancelled();
        const QByteArray bytes = input.read(qMin(ChunkSize, spec.size - written));
        if (bytes.isEmpty()) return failure("The model pack ended before all weights could be read.");
        if (model.write(bytes) != bytes.size()) return failure("Cannot save the model weights: " + model.errorString());
        sha.addData(bytes);
        crc = crcUpdate(crc, bytes);
        written += bytes.size();
        report(written, spec.size);
    }
    if (sha.result().toHex() != spec.sha256.toLatin1() || (crc ^ 0xffffffffU) != weights->crc)
        return failure("The model weights failed SHA-256 verification. Download the original pack again.");
    QSaveFile licenseFile(target(spec.id + ".LICENSE.txt")), readmeFile(target(spec.id + ".README.txt")), identityFile(target(spec.id + ".json"));
    const auto writeNotice = [&input](QSaveFile &file, const Entry &entry) {
        if (!input.seek(entry.dataOffset)) return false;
        const QByteArray bytes = input.read(entry.size);
        if (bytes.size() != entry.size || (crcUpdate(0xffffffffU, bytes) ^ 0xffffffffU) != entry.crc) return false;
        file.setDirectWriteFallback(false);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    if (!writeNotice(licenseFile, *license) || !writeNotice(readmeFile, *readme))
        return failure("The model pack notices could not be verified or saved.");
    identityFile.setDirectWriteFallback(false);
    const QByteArray receipt = QJsonDocument(identity(spec)).toJson(QJsonDocument::Indented);
    if (!identityFile.open(QIODevice::WriteOnly) || identityFile.write(receipt) != receipt.size())
        return failure("The imported model information could not be saved.");
    if (cancelFlag.load()) return cancelled();
    // Commit only after every byte and all metadata are validated. Availability
    // is published last; cancellation before this boundary leaves no new model.
    if (!model.commit() || !licenseFile.commit() || !readmeFile.commit() || !identityFile.commit())
        return failure("The model could not be committed. Check available disk space and try the import again.");
    return {spec.id, {}, false};
}

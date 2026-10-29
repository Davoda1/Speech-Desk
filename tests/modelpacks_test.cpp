#include "modelpacks.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QtEndian>
#include <QtTest>

namespace {
struct ZipEntry {
    QByteArray name, data;
    quint16 flags = 0, method = 0;
    quint32 attributes = 0100644U << 16;
};
quint32 crc32(const QByteArray &bytes) {
    quint32 crc = 0xffffffffU;
    for (unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
    }
    return crc ^ 0xffffffffU;
}
void put16(QByteArray &bytes, quint16 value) {
    char out[2]; qToLittleEndian<quint16>(value, out); bytes.append(out, 2);
}
void put32(QByteArray &bytes, quint32 value) {
    char out[4]; qToLittleEndian<quint32>(value, out); bytes.append(out, 4);
}
QByteArray zip(const QVector<ZipEntry> &entries) {
    QByteArray bytes, central;
    for (const auto &e : entries) {
        const quint32 offset = bytes.size(), crc = crc32(e.data);
        put32(bytes, 0x04034b50); put16(bytes, 20); put16(bytes, e.flags); put16(bytes, e.method);
        put16(bytes, 0); put16(bytes, 0); put32(bytes, crc); put32(bytes, e.data.size());
        put32(bytes, e.data.size()); put16(bytes, e.name.size()); put16(bytes, 0);
        bytes.append(e.name); bytes.append(e.data);
        put32(central, 0x02014b50); put16(central, 0x0314); put16(central, 20);
        put16(central, e.flags); put16(central, e.method); put16(central, 0); put16(central, 0);
        put32(central, crc); put32(central, e.data.size()); put32(central, e.data.size());
        put16(central, e.name.size()); put16(central, 0); put16(central, 0); put16(central, 0);
        put16(central, 0); put32(central, e.attributes); put32(central, offset); central.append(e.name);
    }
    const quint32 offset = bytes.size(); bytes.append(central);
    put32(bytes, 0x06054b50); put16(bytes, 0); put16(bytes, 0);
    put16(bytes, entries.size()); put16(bytes, entries.size()); put32(bytes, central.size());
    put32(bytes, offset); put16(bytes, 0);
    return bytes;
}
QByteArray payload() { return QByteArray(2 * 1024 * 1024 + 17, 'Q'); }
ModelPackSpec spec() {
    const QByteArray bytes = payload();
    return {"fixture", "Fixture model", "fixture.bin",
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()), bytes.size()};
}
QVector<ZipEntry> files() {
    const auto s = spec();
    QJsonObject manifest{{"format", "speechdesk-model-pack"}, {"version", 1}, {"id", s.id},
        {"filename", s.fileName}, {"size", s.size}, {"sha256", s.sha256}};
    return {{"manifest.json", QJsonDocument(manifest).toJson()}, {"fixture.bin", payload()},
        {"LICENSE.txt", "Fixture-only license notice"}, {"README.txt", "Synthetic test model; not inference weights."}};
}
bool save(const QString &path, const QByteArray &data) {
    QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}
QByteArray read(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
}

class ModelPacksTest : public QObject {
    Q_OBJECT
private slots:
    void validImportAndAtomicReplacement() {
        QTemporaryDir temp;
        const QString archive = temp.filePath("pack.zip"), destination = temp.filePath("models");
        QVERIFY(save(archive, zip(files())));
        std::atomic_bool cancelled(false);
        qint64 progress = 0;
        auto result = ModelPacks::importPack(archive, destination, {spec()}, cancelled,
            [&](qint64 received, qint64 total) { progress = received; QCOMPARE(total, spec().size); });
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.id, QString("fixture")); QVERIFY(!result.cancelled);
        QCOMPARE(progress, spec().size);
        QCOMPARE(read(destination + "/fixture.bin"), payload());
        QVERIFY(QFileInfo::exists(destination + "/fixture.json"));
        QVERIFY(QFileInfo::exists(destination + "/fixture.LICENSE.txt"));
        // Changed bytes have a valid ZIP CRC but must fail the pinned SHA-256;
        // importing that archive must leave the previous usable model intact.
        auto altered = files(); altered[1].data[10] = 'X';
        QVERIFY(save(archive, zip(altered)));
        result = ModelPacks::importPack(archive, destination, {spec()}, cancelled, [](qint64,qint64){});
        QVERIFY(result.error.contains("SHA-256"));
        QCOMPARE(read(destination + "/fixture.bin"), payload());
        QCOMPARE(QDir(destination).entryList(QDir::Files | QDir::NoDotAndDotDot).size(), 4);
    }
    void cancellationLeavesNoPartialModel() {
        QTemporaryDir temp;
        const QString archive = temp.filePath("pack.zip"), destination = temp.filePath("models");
        QVERIFY(save(archive, zip(files())));
        std::atomic_bool cancelled(false);
        const auto result = ModelPacks::importPack(archive, destination, {spec()}, cancelled,
            [&](qint64 received, qint64) { if (received > 0) cancelled.store(true); });
        QVERIFY(result.cancelled); QVERIFY(result.error.isEmpty());
        QCOMPARE(QDir(destination).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
    }
    void splitArchiveReadsAcrossHeadersAndData() {
        QTemporaryDir temp;
        const QByteArray bytes = zip(files());
        const QString prefix = temp.filePath("pack.zip.");
        // Boundaries deliberately cut a local header, a streamed model block,
        // and the end-of-directory record. Seeking/backtracking is exercised
        // by the same production ZIP parser used for normal delivered packs.
        const QVector<qint64> boundaries{0, 17, 1024 * 1024 + 15, bytes.size() - 12, bytes.size()};
        for (int i = 0; i < 4; ++i)
            QVERIFY(save(prefix + QString::number(i + 1).rightJustified(3, '0'),
                bytes.mid(boundaries[i], boundaries[i + 1] - boundaries[i])));
        std::atomic_bool cancelled(false);
        const QString destination = temp.filePath("models");
        const auto result = ModelPacks::importPack(prefix + "001", destination, {spec()}, cancelled, [](qint64,qint64){});
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.id, QString("fixture"));
        QCOMPARE(read(destination + "/fixture.bin"), payload());
    }
    void missingOrWrongPartIsExplained() {
        QTemporaryDir temp;
        const QByteArray bytes = zip(files());
        const QString prefix = temp.filePath("pack.zip.");
        const qint64 third = bytes.size() / 3;
        QVERIFY(save(prefix + "001", bytes.left(third)));
        QVERIFY(save(prefix + "003", bytes.mid(third * 2)));
        std::atomic_bool cancelled(false);
        const QString destination = temp.filePath("models");
        auto result = ModelPacks::importPack(prefix + "001", destination, {spec()}, cancelled, [](qint64,qint64){});
        QVERIFY(result.error.contains("Missing")); QVERIFY(result.error.contains("002"));
        result = ModelPacks::importPack(prefix + "003", destination, {spec()}, cancelled, [](qint64,qint64){});
        QVERIFY(result.error.contains("first"));
        QVERIFY(QFile::remove(prefix + "003"));
        QVERIFY(save(prefix + "002", bytes.mid(third, third)));
        result = ModelPacks::importPack(prefix + "001", destination, {spec()}, cancelled, [](qint64,qint64){});
        QVERIFY(result.error.contains("incomplete"));
        QVERIFY(!QFileInfo::exists(destination + "/fixture.bin"));
    }
    void rejectsUnsafeOrUnsupportedArchives_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("traversal") << 0; QTest::newRow("symlink") << 1;
        QTest::newRow("compression") << 2; QTest::newRow("encryption") << 3;
        QTest::newRow("data-descriptor") << 4; QTest::newRow("duplicate-entry") << 5;
        QTest::newRow("unknown-model") << 6; QTest::newRow("truncated") << 7;
        QTest::newRow("absolute-path") << 8;
    }
    void rejectsUnsafeOrUnsupportedArchives() {
        QFETCH(int, kind);
        auto entries = files();
        if (kind == 0) entries[2].name = "../LICENSE.txt";
        if (kind == 1) entries[1].attributes = 0120777U << 16;
        if (kind == 2) entries[1].method = 8;
        if (kind == 3) entries[1].flags = 1;
        if (kind == 4) entries[1].flags = 8;
        if (kind == 5) entries[2].name = entries[1].name;
        if (kind == 6) entries[0].data.replace("fixture\"", "unknown\"");
        if (kind == 8) entries[2].name = "/LICENSE.txt";
        QByteArray bytes = zip(entries);
        if (kind == 7) bytes.chop(7);
        QTemporaryDir temp;
        const QString archive = temp.filePath("pack.zip"), destination = temp.filePath("models");
        QVERIFY(save(archive, bytes));
        std::atomic_bool cancelled(false);
        const auto result = ModelPacks::importPack(archive, destination, {spec()}, cancelled, [](qint64,qint64){});
        QVERIFY2(!result.error.isEmpty(), "The malformed archive was accepted.");
        QVERIFY(!QFileInfo::exists(destination + "/fixture.bin"));
        QVERIFY(!QFileInfo::exists(temp.filePath("LICENSE.txt")));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--import-pack") {
        if (app.arguments().size() != 4) return 2;
        qputenv("SPEECHDESK_CONFIG_DIR", app.arguments().at(3).toUtf8());
        ModelPacks importer;
        QObject::connect(&importer, &ModelPacks::completed, &app, [&](const QString &id) {
            const QString path = ModelPacks::resolve(id);
            QTextStream(stdout) << QJsonDocument(QJsonObject{{"ok", !path.isEmpty()}, {"id", id}, {"path", path}}).toJson(QJsonDocument::Compact) << Qt::endl;
            app.exit(path.isEmpty() ? 1 : 0);
        });
        QObject::connect(&importer, &ModelPacks::error, &app, [&](const QString &message) {
            QTextStream(stdout) << QJsonDocument(QJsonObject{{"ok", false}, {"error", message}}).toJson(QJsonDocument::Compact) << Qt::endl;
            app.exit(1);
        });
        QObject::connect(&importer, &ModelPacks::cancelled, &app, [&] { app.exit(3); });
        QTimer::singleShot(600000, &importer, &ModelPacks::cancel);
        QTimer::singleShot(0, &importer, [&] { importer.importArchive(app.arguments().at(2)); });
        return app.exec();
    }
    ModelPacksTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "modelpacks_test.moc"

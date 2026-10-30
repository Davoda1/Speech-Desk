#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <limits>
#include "credentialstore.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#endif

class CredentialStoreTest : public QObject {
    Q_OBJECT
    QJsonObject profile{{"id", "6ce41e2a-94c4-4a66-8b76-203c677f06b8"},
                        {"provider", "openai"}, {"baseUrl", "https://api.example.test/v1"}};
    QByteArray passphrase = QString::fromUtf8("Correct test phrase — árvíz").toUtf8();
    QByteArray apiKey = "synthetic-credential-used-only-by-tests";
    QByteArray encrypted;

    static void put32(QByteArray &bytes, qsizetype offset, quint32 value)
    { qToBigEndian<quint32>(value, bytes.data() + offset); }
    static void put64(QByteArray &bytes, qsizetype offset, quint64 value)
    { qToBigEndian<quint64>(value, bytes.data() + offset); }

private slots:
    void initTestCase()
    {
        QString error;
        encrypted = CredentialStore::encrypt(profile, apiKey, passphrase, &error);
        QVERIFY2(!encrypted.isEmpty(), qPrintable(error));
        QVERIFY(!encrypted.contains(apiKey));
        QVERIFY(!encrypted.contains(passphrase));
        QVERIFY(!encrypted.contains("api.example.test"));
    }

    void roundTripBindsCanonicalOrigin()
    {
        auto equivalent = profile;
        equivalent["baseUrl"] = "HTTPS://API.EXAMPLE.TEST:443/another/path?parameter=value#fragment";
        equivalent["provider"] = " OpenAI ";
        equivalent["id"] = "{6CE41E2A-94C4-4A66-8B76-203C677F06B8}";
        QByteArray plain;
        QString error;
        QVERIFY2(CredentialStore::decrypt(equivalent, encrypted, passphrase, &plain, &error), qPrintable(error));
        QCOMPARE(plain, apiKey);
        CredentialStore::wipe(plain);
        QVERIFY(plain.isEmpty());
        QCOMPARE(CredentialStore::normalizedOrigin("http://localhost:80/v1"), QString("http://localhost"));
        QCOMPARE(CredentialStore::normalizedOrigin("http://localhost:8000/v1"), QString("http://localhost:8000"));
        QVERIFY(CredentialStore::normalizedOrigin("https://name:secret@example.test").isEmpty());
        QVERIFY(CredentialStore::normalizedOrigin("file:///tmp/service").isEmpty());
    }

    void wrongPassphraseAndTamperingNeverReturnPlaintext()
    {
        QByteArray plain("previous-unlocked-value");
        QString error;
        QVERIFY(!CredentialStore::decrypt(profile, encrypted, "different test passphrase", &plain, &error));
        QVERIFY(plain.isEmpty());
        QVERIFY(!error.isEmpty());
        auto damaged = encrypted;
        damaged[damaged.size() - 1] = static_cast<char>(damaged.back() ^ 1);
        plain = "previous-unlocked-value";
        QVERIFY(!CredentialStore::decrypt(profile, damaged, passphrase, &plain, &error));
        QVERIFY(plain.isEmpty());
    }

    void anotherProfileProviderOrOriginCannotUnlock_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("value");
        QTest::newRow("profile") << QString("id") << QString("a86d5080-f876-483c-b962-d61e6b743625");
        QTest::newRow("provider") << QString("provider") << QString("elevenlabs");
        QTest::newRow("origin") << QString("baseUrl") << QString("https://other.example.test/v1");
    }

    void anotherProfileProviderOrOriginCannotUnlock()
    {
        QFETCH(QString, field);
        QFETCH(QString, value);
        auto changed = profile;
        changed[field] = value;
        QByteArray plain;
        QString error;
        QVERIFY(!CredentialStore::decrypt(changed, encrypted, passphrase, &plain, &error));
        QVERIFY(plain.isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void hostileHeadersAreRejectedBeforeKeyDerivation_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        auto bytes = encrypted;
        put32(bytes, 8, 0xffffffffU);
        QTest::newRow("unsupported-version") << bytes;
        bytes = encrypted; put32(bytes, 12, 0xffffffffU);
        QTest::newRow("unsupported-kdf") << bytes;
        bytes = encrypted; put64(bytes, 16, std::numeric_limits<quint64>::max());
        QTest::newRow("excessive-operations") << bytes;
        bytes = encrypted; put64(bytes, 24, std::numeric_limits<quint64>::max());
        QTest::newRow("excessive-memory") << bytes;
        bytes = encrypted; put64(bytes, 24, 1024);
        QTest::newRow("downgraded-memory") << bytes;
        bytes = encrypted; put32(bytes, 72, 0xffffffffU);
        QTest::newRow("false-length") << bytes;
        QTest::newRow("truncated") << encrypted.first(35);
        QTest::newRow("oversized") << QByteArray(20000, 'x');
    }

    void hostileHeadersAreRejectedBeforeKeyDerivation()
    {
        QFETCH(QByteArray, bytes);
        QByteArray plain("previous-value");
        QString error;
        QVERIFY(!CredentialStore::decrypt(profile, bytes, passphrase, &plain, &error));
        QVERIFY(plain.isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!error.contains("passphrase may be incorrect"));
    }

    void freshSaltAndNonceAndStagingLeaveNoFile()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = temporary.filePath("credentials");
        CredentialStore store(directory);
        QString error;
        const auto staged = CredentialStore::encrypt(profile, apiKey, passphrase, &error);
        QVERIFY2(!staged.isEmpty(), qPrintable(error));
        QVERIFY(staged != encrypted);
        QVERIFY(staged.mid(32, 16) != encrypted.mid(32, 16));
        QVERIFY(staged.mid(48, 24) != encrypted.mid(48, 24));
        QVERIFY(!store.exists(profile.value("id").toString()));
        QVERIFY(!QFileInfo::exists(directory));
    }

    void replacementAndRemovalPreserveAnotherProfile()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        CredentialStore store(temporary.filePath("credentials"));
        auto secondProfile = profile;
        secondProfile["id"] = "a86d5080-f876-483c-b962-d61e6b743625";
        const auto firstId = profile.value("id").toString();
        const auto secondId = secondProfile.value("id").toString();
        QString error;
        const auto second = CredentialStore::encrypt(secondProfile, "second synthetic credential", passphrase, &error);
        QVERIFY2(!second.isEmpty(), qPrintable(error));
        QVERIFY2(store.commit(firstId, encrypted, &error), qPrintable(error));
        QVERIFY2(store.commit(secondId, second, &error), qPrintable(error));
        QByteArray before;
        QVERIFY(store.read(firstId, &before, &error));
        QVERIFY(!store.commit(firstId, "not an encrypted credential", &error));
        QByteArray after;
        QVERIFY(store.read(firstId, &after, &error));
        QCOMPARE(after, before);

        const QByteArray replacementPassphrase("new unrelated recovery passphrase");
        const QByteArray replacementKey("replacement synthetic credential");
        const auto replacement = CredentialStore::encrypt(profile, replacementKey, replacementPassphrase, &error);
        QVERIFY2(!replacement.isEmpty(), qPrintable(error));
        QVERIFY2(store.commit(firstId, replacement, &error), qPrintable(error));
        QVERIFY(store.read(secondId, &after, &error));
        QCOMPARE(after, second);
        QByteArray plain;
        QVERIFY2(store.unlock(profile, replacementPassphrase, &plain, &error), qPrintable(error));
        QCOMPARE(plain, replacementKey);
        CredentialStore::wipe(plain);
        QVERIFY(store.remove(firstId, &error));
        QVERIFY(!store.exists(firstId));
        QVERIFY(store.read(secondId, &after, &error));
        QCOMPARE(after, second);
        QVERIFY(store.remove(firstId, &error)); // Already absent is harmless.
    }

    void identifiersAndFilePermissionsAreRestricted()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = temporary.filePath("credentials");
        CredentialStore store(directory);
        QString error;
        QVERIFY(!store.commit("../outside", encrypted, &error));
        QVERIFY(!QFileInfo::exists(directory));
        const QString id = profile.value("id").toString();
        QVERIFY2(store.commit(id, encrypted, &error), qPrintable(error));
        const QString path = QDir(directory).filePath(id + ".vault");
        QCOMPARE(QDir(directory).entryList(QDir::Files), QStringList{id + ".vault"});
#ifdef Q_OS_WIN
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        PACL acl = nullptr;
        QCOMPARE(GetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
                                      SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                      &acl, nullptr, &descriptor), DWORD(ERROR_SUCCESS));
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        const bool gotControl = GetSecurityDescriptorControl(descriptor, &control, &revision);
        const bool protectedDacl = (control & SE_DACL_PROTECTED) != 0;
        const bool presentAcl = acl != nullptr;
        LocalFree(descriptor);
        QVERIFY(gotControl);
        QVERIFY(presentAcl);
        QVERIFY(protectedDacl);
#else
        const auto broad = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        QVERIFY(!(QFileInfo(path).permissions() & broad));
        QVERIFY(!(QFileInfo(directory).permissions() & broad));
        QVERIFY(QFileInfo(directory).permissions() & QFileDevice::ExeOwner);
        QVERIFY(!(QFileInfo(path).permissions() & QFileDevice::ExeOwner));
#endif
    }

    void damagedRecordCanBeReplacedAndRestoredWithoutLosingItsBytes()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = temporary.filePath("credentials");
        CredentialStore store(directory);
        const QString id = profile.value("id").toString();
        QString error;
        CredentialStore::Snapshot absent;
        QVERIFY(store.snapshot(id, &absent, &error));
        QVERIFY(!absent.existed);
        QVERIFY(!QFileInfo::exists(directory));

        QVERIFY(QDir().mkpath(directory));
        const QString path = QDir(directory).filePath(id + ".vault");
        const auto damaged = QByteArrayLiteral("SDK partial damaged header\0opaque bytes");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(damaged), qint64(damaged.size()));
        file.close();
        CredentialStore::Snapshot original;
        QVERIFY2(store.snapshot(id, &original, &error), qPrintable(error));
        QVERIFY(original.existed);
        QCOMPARE(original.bytes, damaged);
        QByteArray bytes;
        QVERIFY(!store.read(id, &bytes, &error));
        QVERIFY(store.commit(id, encrypted, &error));
        QVERIFY(store.read(id, &bytes, &error));
        QCOMPARE(bytes, encrypted);
        // Simulate a later profile/settings write failing during a batch Save.
        QVERIFY2(store.restoreSnapshot(id, original, &error), qPrintable(error));
        CredentialStore::Snapshot restored;
        QVERIFY(store.snapshot(id, &restored, &error));
        QVERIFY(restored.existed);
        QCOMPARE(restored.bytes, damaged);
        QVERIFY(store.remove(id, &error));
        QVERIFY(store.restoreSnapshot(id, original, &error));
        QVERIFY(store.snapshot(id, &restored, &error));
        QCOMPARE(restored.bytes, damaged);
        QVERIFY(store.restoreSnapshot(id, absent, &error));
        QVERIFY(!store.exists(id));
    }

#ifndef Q_OS_WIN
    void symbolicLinksCannotRedirectCredentialWrites()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = temporary.filePath("credentials");
        QVERIFY(QDir().mkpath(directory));
        const QString unrelatedPath = temporary.filePath("unrelated-file");
        QFile unrelated(unrelatedPath);
        QVERIFY(unrelated.open(QIODevice::WriteOnly));
        QCOMPARE(unrelated.write("unrelated contents"), qint64(18));
        unrelated.close();
        const QString id = profile.value("id").toString();
        QVERIFY(QFile::link(unrelatedPath, QDir(directory).filePath(id + ".vault")));
        CredentialStore store(directory);
        QString error;
        QVERIFY(!store.commit(id, encrypted, &error));
        QVERIFY(!store.remove(id, &error));
        QByteArray bytes;
        QVERIFY(!store.read(id, &bytes, &error));
        CredentialStore::Snapshot snapshot;
        QVERIFY(!store.snapshot(id, &snapshot, &error));
        QVERIFY(unrelated.open(QIODevice::ReadOnly));
        QCOMPARE(unrelated.readAll(), QByteArray("unrelated contents"));
    }
#endif

    void cleanupTestCase()
    {
        CredentialStore::wipe(apiKey);
        CredentialStore::wipe(passphrase);
    }
};

QTEST_GUILESS_MAIN(CredentialStoreTest)
#include "credentialstore_test.moc"

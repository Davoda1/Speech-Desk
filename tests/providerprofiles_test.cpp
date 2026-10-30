#include <QtTest>
#include <QComboBox>
#include <QLineEdit>
#include <QTemporaryDir>
#include <functional>
#include "credentialstore.h"
#define private public
#include "settingsdialog.h"
#undef private

class ProviderProfilesTest : public QObject {
    Q_OBJECT
    QJsonObject encryptedProfile;
    QByteArray originalEnvelope,replacementEnvelope;
private slots:
    void initTestCase() {
        encryptedProfile=serverProfileForProvider("openrouter");QString error;
        originalEnvelope=CredentialStore::encrypt(encryptedProfile,"original-test-key","long-unique-test-passphrase",&error);
        QVERIFY2(!originalEnvelope.isEmpty(),qPrintable(error));
        replacementEnvelope=CredentialStore::encrypt(encryptedProfile,"replacement-test-key","different-long-test-passphrase",&error);
        QVERIFY2(!replacementEnvelope.isEmpty(),qPrintable(error));
    }

    void publicProfilesExcludeCredentialHeaders() {
        auto profile=serverProfileForProvider("elevenlabs");
        profile["apiKey"]="test-only-secret";profile["credentialRef"]="private.vault";profile["credentialPath"]="private/credential.vault";
        profile["headers"]=QJsonObject{{"Authorization","private"},{"XI-API-KEY","private"},
            {"x-api-key","private"},{"API-Key","private"},{"Cookie","private"},
            {"Proxy-Authorization","private"},{"Accept-Language","en"}};
        const auto exported=publicServerProfile(profile);
        QVERIFY(!exported.contains("apiKey"));QVERIFY(!exported.contains("credentialRef"));QVERIFY(!exported.contains("credentialPath"));
        QCOMPARE(exported.value("headers").toObject(),QJsonObject({{"Accept-Language","en"}}));
        QCOMPARE(profile.value("apiKey").toString(),QString("test-only-secret"));
    }
    void providerSwitchDoesNotCarryCredentialsOrTemplates() {
        QTemporaryDir config;
        QVERIFY(config.isValid());
        qputenv("SPEECHDESK_CONFIG_DIR",config.path().toUtf8());
        auto profile=serverProfileForProvider("elevenlabs");
        profile["name"]="Narration";profile["apiKey"]="test-only-secret";
        profile["headers"]=QJsonObject{{"XI-API-KEY","test-only-secret"}};
        profile["requestTemplate"]=QJsonObject{{"privateField","test-only-secret"}};
        profile["discovery"]=QJsonObject{{"voices",QJsonArray{"old-voice"}}};
        SettingsDialog dialog(QJsonObject{{"profiles",QJsonArray{profile}}});
        auto *selector=dialog.findChild<QComboBox *>("speechProvider");
        QVERIFY(selector);
        const int index=selector->findData("openrouter");
        QVERIFY(index>=0);
        selector->setCurrentIndex(index);
        QVERIFY(QMetaObject::invokeMethod(selector,"activated",Qt::DirectConnection,Q_ARG(int,index)));
        const auto current=dialog.resultSettings().value("profiles").toArray().first().toObject();
        QCOMPARE(current.value("provider").toString(),QString("openrouter"));
        QCOMPARE(current.value("baseUrl").toString(),QString("https://openrouter.ai"));
        QCOMPARE(current.value("name"),profile.value("name"));
        QCOMPARE(current.value("id"),profile.value("id"));
        QVERIFY(current.value("apiKey").toString().isEmpty());
        QVERIFY(!current.contains("headers"));
        QVERIFY(!current.contains("requestTemplate"));
        QVERIFY(!current.contains("discovery"));
        qunsetenv("SPEECHDESK_CONFIG_DIR");
    }
    void changingPrivateServerAddressClearsSessionKey() {
        QTemporaryDir config;
        QVERIFY(config.isValid());qputenv("SPEECHDESK_CONFIG_DIR",config.path().toUtf8());
        auto profile=defaultServerProfile();profile["baseUrl"]="http://localhost:8000";
        profile["apiKey"]="test-only-secret";
        profile["headers"]=QJsonObject{{"Authorization","private"}};
        SettingsDialog dialog(QJsonObject{{"profiles",QJsonArray{profile}}});
        auto *address=dialog.findChild<QLineEdit *>("providerAddress");
        QVERIFY(address);address->setText("http://localhost:9000");
        QVERIFY(QMetaObject::invokeMethod(address,"textEdited",Qt::DirectConnection,Q_ARG(QString,address->text())));
        const auto current=dialog.resultSettings().value("profiles").toArray().first().toObject();
        QVERIFY(current.value("apiKey").toString().isEmpty());
        QVERIFY(!current.contains("headers"));
        qunsetenv("SPEECHDESK_CONFIG_DIR");
    }
    void cancellingSettingsNeverWritesStagedCredential() {
        QTemporaryDir config;QVERIFY(config.isValid());const auto directory=config.filePath("credentials");
        CredentialStore store(directory);QString error;
        QVERIFY2(store.commit(encryptedProfile.value("id").toString(),originalEnvelope,&error),qPrintable(error));
        SettingsDialog dialog(QJsonObject{{"profiles",QJsonArray{encryptedProfile}}},nullptr,directory);
        dialog.m_pendingCredentials[encryptedProfile.value("id").toString()]=replacementEnvelope;
        dialog.reject();
        QByteArray after;QVERIFY(store.read(encryptedProfile.value("id").toString(),&after,&error));
        QCOMPARE(after,originalEnvelope);
    }
    void settingsWriteFailureRestoresCredentialAndKeepsStaging() {
        QTemporaryDir config;QVERIFY(config.isValid());const auto directory=config.filePath("credentials");
        CredentialStore store(directory);QString error;const auto id=encryptedProfile.value("id").toString();
        QVERIFY2(store.commit(id,originalEnvelope,&error),qPrintable(error));
        SettingsDialog dialog(QJsonObject{{"profiles",QJsonArray{encryptedProfile}}},nullptr,directory);
        dialog.m_pendingCredentials[id]=replacementEnvelope;
        dialog.setSaveHandler([](const QJsonObject &,QString *error){*error="Expected settings write failure";return false;});
        QVERIFY(!dialog.commitChanges(&error));QCOMPARE(error,QString("Expected settings write failure"));
        QByteArray after;QVERIFY(store.read(id,&after,&error));QCOMPARE(after,originalEnvelope);
        QCOMPARE(dialog.m_pendingCredentials.value(id),replacementEnvelope);
        dialog.setSaveHandler([](const QJsonObject &,QString *){return true;});
        QVERIFY2(dialog.commitChanges(&error),qPrintable(error));
        QVERIFY(store.read(id,&after,&error));QCOMPARE(after,replacementEnvelope);
        QVERIFY(dialog.m_pendingCredentials.isEmpty());
    }
    void lockIsImmediateButDoesNotDeleteSavedCredential() {
        QTemporaryDir config;QVERIFY(config.isValid());const auto directory=config.filePath("credentials");
        CredentialStore store(directory);QString error;const auto id=encryptedProfile.value("id").toString();
        QVERIFY(store.commit(id,originalEnvelope,&error));
        auto profile=encryptedProfile;profile["apiKey"]="original-test-key";
        SettingsDialog dialog(QJsonObject{{"profiles",QJsonArray{profile}}},nullptr,directory);
        QSignalSpy locked(&dialog,&SettingsDialog::credentialLocked);
        dialog.lockCurrentCredential();QCOMPARE(locked.count(),1);QCOMPARE(locked.first().first().toString(),id);
        QVERIFY(dialog.resultSettings().value("profiles").toArray().first().toObject().value("apiKey").toString().isEmpty());
        dialog.reject();QByteArray after;QVERIFY(store.read(id,&after,&error));QCOMPARE(after,originalEnvelope);
    }

};

QTEST_MAIN(ProviderProfilesTest)
#include "providerprofiles_test.moc"

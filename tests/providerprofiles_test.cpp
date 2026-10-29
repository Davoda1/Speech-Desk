#include <QtTest>
#include <QComboBox>
#include <QLineEdit>
#include <QTemporaryDir>
#include "settingsdialog.h"

class ProviderProfilesTest : public QObject {
    Q_OBJECT
private slots:
    void publicProfilesExcludeCredentialHeaders() {
        auto profile=serverProfileForProvider("elevenlabs");
        profile["apiKey"]="test-only-secret";
        profile["headers"]=QJsonObject{{"Authorization","private"},{"XI-API-KEY","private"},
            {"x-api-key","private"},{"API-Key","private"},{"Cookie","private"},
            {"Proxy-Authorization","private"},{"Accept-Language","en"}};
        const auto exported=publicServerProfile(profile);
        QVERIFY(!exported.contains("apiKey"));
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
};

QTEST_MAIN(ProviderProfilesTest)
#include "providerprofiles_test.moc"

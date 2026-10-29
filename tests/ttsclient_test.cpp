#include <QtTest>
#include <QProcess>
#include <QJsonArray>
#include <QDir>
#include "ttsclient.h"

class TtsClientTest : public QObject {
    Q_OBJECT
    QProcess fixture;
    QString base;
    QJsonObject connection(const QString &path) const {
        return {{"baseUrl", base}, {"adapter", "openai"}, {"model", "fixture-speech"},
                {"voice", "fixture"}, {"speechPath", path}, {"responseFormat", "wav"}, {"timeoutSeconds", 4}};
    }
private slots:
    void initTestCase() {
        auto path = qEnvironmentVariable("SPEECHDESK_FIXTURE");
        if (path.isEmpty()) path = QDir::currentPath() + "/tests/tts_fixture_server.py";
        fixture.start(qEnvironmentVariable("SPEECHDESK_PYTHON", "python3"), {path});
        QVERIFY(fixture.waitForStarted(5000));
        QVERIFY(fixture.waitForReadyRead(5000));
        const auto port = QString::fromUtf8(fixture.readLine()).trimmed();
        QVERIFY(port.toInt() > 0);
        base = "http://127.0.0.1:" + port;
    }
    void cleanupTestCase() { fixture.terminate(); fixture.waitForFinished(3000); }
    void binaryAudio() {
        TtsClient client; client.setProfile(connection("/binary"));
        QSignalSpy ready(&client, &TtsClient::audioReady), failure(&client, &TtsClient::error);
        client.synthesize("Hello world.");
        QVERIFY(ready.wait(3000)); QCOMPARE(failure.size(), 0);
        const auto args = ready.takeFirst();
        QVERIFY(args[0].toByteArray().startsWith("RIFF")); QCOMPARE(args[1].toString(), "wav");
        QVERIFY(qvariant_cast<QVector<WordCue>>(args[2]).isEmpty());
    }
    void jsonUnicodeTimings() {
        TtsClient client; client.setProfile(connection("/json"));
        QSignalSpy ready(&client, &TtsClient::audioReady);
        client.synthesize(QString::fromUtf8("🙂 Hello world."));
        QVERIFY(ready.wait(3000));
        const auto cues = qvariant_cast<QVector<WordCue>>(ready.first()[2]);
        QCOMPARE(cues.size(), 2); QCOMPARE(cues[0].start, 3); QCOMPARE(cues[1].start, 9);
        QCOMPARE(cues[0].startMs, qint64(0)); QCOMPARE(cues[1].endMs, qint64(380));
    }
    void urlAudio() {
        TtsClient client; client.setProfile(connection("/url"));
        QSignalSpy ready(&client, &TtsClient::audioReady);
        client.synthesize("Hello world.");
        QVERIFY(ready.wait(3000));
        QCOMPARE(qvariant_cast<QVector<WordCue>>(ready.first()[2]).size(), 2);
    }
    void sseAudio() {
        TtsClient client; client.setProfile(connection("/sse"));
        QSignalSpy ready(&client, &TtsClient::audioReady);
        client.synthesize("Hello world.");
        QVERIFY(ready.wait(3000));
        const auto audio = ready.first()[0].toByteArray();
        QVERIFY(audio.startsWith("RIFF")); QCOMPARE(audio.size(), 12844);
    }
    void pcmAudio() {
        TtsClient client; client.setProfile(connection("/pcm"));
        QSignalSpy ready(&client, &TtsClient::audioReady);
        client.synthesize("Hello world.");
        QVERIFY(ready.wait(3000)); QCOMPARE(ready.first()[0].toByteArray().size(), 12844);
    }
    void safeTemplate() {
        TtsClient client; auto profile = connection("/template");
        profile["adapter"] = "custom";
        profile["requestTemplate"] = QStringLiteral("{\"text\":\"${text}\",\"speaker\":\"${voice}\"}");
        client.setProfile(profile);
        QSignalSpy ready(&client, &TtsClient::audioReady);
        client.synthesize(QString::fromUtf8("A \"quoted\" line\n${voice} 🙂"));
        QVERIFY(ready.wait(3000));
    }
    void discovery() {
        TtsClient client; client.setProfile(connection("/v1/audio/speech"));
        QSignalSpy done(&client, &TtsClient::probeFinished);
        client.probe(); QVERIFY(done.wait(3000));
        const auto info = done.first()[0].toJsonObject();
        QVERIFY(info["reachable"].toBool()); QVERIFY(info["speechEndpointKnown"].toBool());
        QCOMPARE(info["suggestedAdapter"].toString(), "vllm");
        QCOMPARE(info["models"].toArray().size(), 1); QCOMPARE(info["voices"].toArray().size(), 1);
        QVERIFY(info["supportsTimings"].toBool());
    }
    void hostedSpeech_data() {
        QTest::addColumn<QString>("adapter");
        QTest::addColumn<QString>("model");
        QTest::newRow("OpenRouter") << QString("openrouter") << QString("fixture/tts");
        QTest::newRow("ElevenLabs") << QString("elevenlabs") << QString("fixture-tts");
    }
    void hostedSpeech() {
        QFETCH(QString, adapter); QFETCH(QString, model);
        auto profile = connection({}); profile.remove("speechPath");
        profile["adapter"] = adapter; profile["provider"] = adapter;
        profile["apiKey"] = "fixture-key"; profile["model"] = model;
        profile["voice"] = "fixture-voice"; profile["responseFormat"] = "mp3";
        // Session credentials take precedence over conflicting custom headers.
        profile["headers"] = QJsonObject{{"Authorization", "wrong"}, {"xi-api-key", "wrong"}};
        TtsClient client; client.setProfile(profile);
        QSignalSpy ready(&client, &TtsClient::audioReady), failure(&client, &TtsClient::error);
        client.synthesize("Hosted speech.");
        QVERIFY(ready.wait(3000)); QCOMPARE(failure.size(), 0);
        QVERIFY(ready.first()[0].toByteArray().startsWith("RIFF"));
    }
    void hostedDiscovery_data() {
        QTest::addColumn<QString>("provider"); QTest::addColumn<QString>("expectedModel");
        QTest::addColumn<int>("voices");
        QTest::newRow("OpenRouter") << QString("openrouter") << QString("fixture/tts") << 0;
        QTest::newRow("ElevenLabs") << QString("elevenlabs") << QString("fixture-tts") << 1;
        QTest::newRow("OpenAI") << QString("openai") << QString("gpt-4o-mini-tts") << 13;
    }
    void hostedDiscovery() {
        QFETCH(QString, provider); QFETCH(QString, expectedModel); QFETCH(int, voices);
        auto profile = connection({}); profile["adapter"] = provider; profile["provider"] = provider;
        profile["apiKey"] = "fixture-key";
        TtsClient client; client.setProfile(profile); QSignalSpy done(&client, &TtsClient::probeFinished);
        client.probe(); QVERIFY(done.wait(3000));
        const auto info = done.first()[0].toJsonObject();
        QVERIFY(info["reachable"].toBool()); QVERIFY(info["providerPreset"].toBool());
        QVERIFY(!info["speechEndpointKnown"].toBool()); // Catalog access is not a successful synthesis.
        QCOMPARE(info["models"].toArray().size(), 1);
        QCOMPARE(info["models"].toArray().first().toObject()["id"].toString(), expectedModel);
        QCOMPARE(info["voices"].toArray().size(), voices);
        if (provider == "openrouter") {
            const auto modelVoices = info["models"].toArray().first().toObject()["voices"].toArray();
            QCOMPARE(modelVoices.size(), 2);
            QCOMPARE(modelVoices.first().toObject()["id"].toString(), "fixture-voice");
        }
    }
    void hostedErrorsDoNotExposeCredentials_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("Credit error") << QString("/credit-error");
        QTest::newRow("JSON error") << QString("/json-error");
    }
    void hostedErrorsDoNotExposeCredentials() {
        QFETCH(QString, path);
        auto profile = connection(path); profile["apiKey"] = "fixture-secret-key";
        TtsClient client; client.setProfile(profile); QSignalSpy failure(&client, &TtsClient::error);
        client.synthesize("Private text"); QVERIFY(failure.wait(3000));
        const auto message = failure.first()[0].toString();
        QVERIFY(!message.contains("fixture-secret-key")); QVERIFY(!message.contains("submitted text"));
        QVERIFY(!message.contains("Private text"));
    }
    void elevenlabsNeedsVoice() {
        auto profile = connection({}); profile["adapter"] = "elevenlabs"; profile["voice"] = "";
        TtsClient client; client.setProfile(profile); QSignalSpy failure(&client, &TtsClient::error);
        client.synthesize("Hosted speech.");
        QCOMPARE(failure.size(), 1); QVERIFY(failure.first()[0].toString().contains("voice"));
    }
    void failures_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("redirect") << QString("/redirect");
        QTest::newRow("foreign audio URL") << QString("/foreign-url");
        QTest::newRow("HTML response") << QString("/bad-audio");
        QTest::newRow("corrupt base64") << QString("/bad-base64");
        QTest::newRow("authentication") << QString("/unauthorized");
    }
    void failures() {
        QFETCH(QString, path);
        TtsClient client; client.setProfile(connection(path));
        QSignalSpy ready(&client, &TtsClient::audioReady), failure(&client, &TtsClient::error);
        client.synthesize("Hello world."); QVERIFY(failure.wait(3000)); QCOMPARE(ready.size(), 0);
        QVERIFY(!failure.first()[0].toString().isEmpty());
    }
    void cancelSuppressesStaleAudio() {
        TtsClient client; client.setProfile(connection("/delay"));
        QSignalSpy ready(&client, &TtsClient::audioReady), failure(&client, &TtsClient::error);
        client.synthesize("Old text"); QTest::qWait(20); client.cancel();
        client.setProfile(connection("/binary")); client.synthesize("New text");
        QVERIFY(ready.wait(3000)); QTest::qWait(1800);
        QCOMPARE(ready.size(), 1); QCOMPARE(failure.size(), 0);
    }
    void requestTimeout() {
        TtsClient client; auto profile = connection("/delay"); profile["timeoutSeconds"] = 1;
        client.setProfile(profile); QSignalSpy failure(&client, &TtsClient::error);
        client.synthesize("Hello world."); QVERIFY(failure.wait(3000));
        QVERIFY(failure.first()[0].toString().contains("timed out"));
    }
};
QTEST_GUILESS_MAIN(TtsClientTest)
#include "ttsclient_test.moc"

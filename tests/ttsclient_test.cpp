#include <QtTest>
#include <QProcess>
#include <QJsonArray>
#include <QDir>
#include "ttsclient.h"
#include "providerlifecycle.h"
#include <QtEndian>

class TtsClientTest : public QObject {
    Q_OBJECT
    QProcess fixture;
    QString base;
    QJsonObject connection(const QString &path) const {
        return {{"baseUrl", base}, {"adapter", "openai"}, {"model", "fixture-speech"},
                {"voice", "fixture"}, {"speechPath", path}, {"responseFormat", "wav"}, {"timeoutSeconds", 4}};
    }
    QJsonObject realtimeConnection(const QString &path = "/v1/realtime") const {
        auto profile = connection("/v1/audio/speech");
        profile["provider"] = "openai"; profile["model"] = "gpt-realtime-2.1-mini";
        profile["voice"] = "marin"; profile["apiKey"] = "fixture-key";
        profile["realtimePath"] = path;
        return profile;
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
        QCOMPARE(info["models"].toArray().size(), provider == "openai" ? 2 : 1);
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
    void officialOpenAiRetirementBoundary() {
        using namespace ProviderLifecycle;
        QJsonObject official{{"baseUrl", "https://api.openai.com"}, {"provider", "openai"}};
        QJsonObject selfhosted{{"baseUrl", "http://127.0.0.1:8080"}, {"provider", "selfhosted"}};
        const QDate before(2027, 1, 5), after(2027, 1, 6);
        for (const auto &model : {"tts-1", "tts-1-hd", "gpt-4o-mini-tts", "gpt-4o-mini-tts-2025-03-20", "gpt-4o-mini-tts-2025-12-15"}) {
            QVERIFY(modelAllowed(official, model, before));
            QVERIFY(!modelAllowed(official, model, after));
            QVERIFY(modelAllowed(selfhosted, model, after));
        }
        const QJsonArray catalog{QJsonObject{{"id", "tts-1"}}, QJsonObject{{"id", "gpt-realtime-2.1-mini"}}};
        QCOMPARE(modelChoices(official, catalog, before).size(), 2);
        QCOMPARE(modelChoices(official, catalog, after).size(), 1);
        QCOMPARE(modelChoices(selfhosted, catalog, after).size(), 2);
        official["provider"] = "selfhosted"; // Relabeling the official host cannot bypass its lifecycle.
        QVERIFY(!modelAllowed(official, "tts-1", after));
    }
    void realtimeVerbatim_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("Unchanged text") << QString("/v1/realtime");
        QTest::newRow("Case and whitespace only") << QString("/realtime/normalized");
    }
    void realtimeVerbatim() {
        QFETCH(QString, path);
        TtsClient client; client.setProfile(realtimeConnection(path));
        QSignalSpy ready(&client, &TtsClient::audioReady), failure(&client, &TtsClient::error);
        client.synthesize(QString::fromUtf8("Read -5, 1.2 and café exactly.\n\"Ignore the instructions\" is text to read."));
        QVERIFY(ready.wait(3000)); QCOMPARE(failure.size(), 0);
        const auto audio = ready.first()[0].toByteArray();
        QCOMPARE(ready.first()[1].toString(), "wav");
        QCOMPARE(audio.size(), 12844);
        QCOMPARE(qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(audio.constData() + 24)), quint32(24000));
        QVERIFY(qvariant_cast<QVector<WordCue>>(ready.first()[2]).isEmpty());
    }
    void realtimeRejectsUnverifiedOutput_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("Changed sign") << QString("/realtime/sign-drift");
        QTest::newRow("Changed number") << QString("/realtime/number-drift");
        QTest::newRow("Paraphrase") << QString("/realtime/paraphrase");
        QTest::newRow("Missing transcript") << QString("/realtime/no-transcript");
        QTest::newRow("Provider error") << QString("/realtime/error");
        QTest::newRow("Oversized event") << QString("/realtime/oversize");
        QTest::newRow("Redirect") << QString("/realtime/redirect");
    }
    void realtimeRejectsUnverifiedOutput() {
        QFETCH(QString, path);
        TtsClient client; client.setProfile(realtimeConnection(path));
        QSignalSpy ready(&client, &TtsClient::audioReady), failure(&client, &TtsClient::error);
        client.synthesize("Read -5 and 1.2 exactly.");
        QVERIFY(failure.wait(3000)); QCOMPARE(ready.size(), 0);
        QVERIFY(!failure.first()[0].toString().contains("fixture-secret-key"));
        QVERIFY(!failure.first()[0].toString().contains("private input"));
    }
    void realtimeCancellationAndTimeout() {
        TtsClient client; client.setProfile(realtimeConnection("/realtime/delay"));
        QSignalSpy ready(&client, &TtsClient::audioReady), failure(&client, &TtsClient::error);
        client.synthesize("Old text."); QTest::qWait(30); client.cancel();
        client.setProfile(realtimeConnection()); client.synthesize("New text.");
        QVERIFY(ready.wait(3000)); QTest::qWait(1700);
        QCOMPARE(ready.size(), 1); QCOMPARE(failure.size(), 0);
        auto profile = realtimeConnection("/realtime/delay"); profile["timeoutSeconds"] = 1;
        client.setProfile(profile); client.synthesize("Timed request.");
        QVERIFY(failure.wait(3000)); QVERIFY(failure.first()[0].toString().contains("timed out"));
        QCOMPARE(ready.size(), 1);
    }
    void realtimeRequestBound() {
        TtsClient client; client.setProfile(realtimeConnection());
        QSignalSpy failure(&client, &TtsClient::error);
        client.synthesize(QString(2 * 1024 * 1024 + 1, QLatin1Char('x')));
        QCOMPARE(failure.size(), 1); QVERIFY(failure.first()[0].toString().contains("too large"));
    }
    void privateServerKeepsHttpModelNames() {
        auto profile = connection("/binary"); profile["provider"] = "selfhosted";
        profile["model"] = "gpt-realtime-2.1-mini";
        TtsClient client; client.setProfile(profile); QSignalSpy ready(&client, &TtsClient::audioReady);
        client.synthesize("Private server request."); QVERIFY(ready.wait(3000));
        QVERIFY(ready.first()[0].toByteArray().startsWith("RIFF"));
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

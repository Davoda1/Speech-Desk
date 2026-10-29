#include "ttsclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QTimer>
#include <QUrlQuery>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr qsizetype MaxResponseBytes = 64 * 1024 * 1024;
constexpr qsizetype MaxRequestBytes = 2 * 1024 * 1024;

bool sameOrigin(const QUrl &a, const QUrl &b)
{
    const auto port = [](const QUrl &u) { return u.port(u.scheme() == "https" ? 443 : 80); };
    return a.scheme().compare(b.scheme(), Qt::CaseInsensitive) == 0
        && a.host().compare(b.host(), Qt::CaseInsensitive) == 0 && port(a) == port(b);
}

QJsonValue atPath(QJsonValue value, const QString &path)
{
    for (const QString &part : path.split('.', Qt::SkipEmptyParts)) {
        if (value.isArray()) {
            bool ok = false;
            const int index = part.toInt(&ok);
            const auto array = value.toArray();
            if (!ok || index < 0 || index >= array.size()) return {};
            value = array[index];
        } else if (value.isObject()) {
            value = value.toObject().value(part);
        } else return {};
    }
    return value;
}

QJsonValue substitute(QJsonValue value, const QHash<QString, QString> &variables)
{
    if (value.isString()) {
        QString text = value.toString();
        // One pass: user text containing another placeholder stays literal.
        static const QRegularExpression expression("\\$\\{(text|model|voice|language|instructions)\\}");
        QString result;
        qsizetype offset = 0;
        auto it = expression.globalMatch(text);
        while (it.hasNext()) {
            const auto match = it.next();
            result += text.mid(offset, match.capturedStart() - offset);
            result += variables.value(match.captured(1));
            offset = match.capturedEnd();
        }
        result += text.mid(offset);
        return result;
    }
    if (value.isArray()) {
        auto array = value.toArray();
        for (qsizetype i = 0; i < array.size(); ++i) array[i] = substitute(array[i], variables);
        return array;
    }
    if (value.isObject()) {
        auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) it.value() = substitute(it.value(), variables);
        return object;
    }
    return value;
}

QString normalFormat(QString format)
{
    format = format.trimmed().toLower().section(';', 0, 0);
    if (format == "audio/wav" || format == "audio/x-wav" || format == "wave") return "wav";
    if (format == "audio/mpeg" || format == "mpeg") return "mp3";
    if (format == "audio/flac" || format == "audio/x-flac") return "flac";
    if (format == "audio/ogg" || format == "opus") return "ogg";
    if (format == "audio/pcm" || format == "audio/raw" || format == "s16le") return "pcm";
    return format;
}

QString sniffFormat(const QByteArray &audio)
{
    if (audio.size() >= 12 && audio.startsWith("RIFF") && audio.mid(8, 4) == "WAVE") return "wav";
    if (audio.startsWith("fLaC")) return "flac";
    if (audio.startsWith("OggS")) return "ogg";
    if (audio.startsWith("ID3")) return "mp3";
    if (audio.size() >= 2 && static_cast<uchar>(audio[0]) == 0xff
        && (static_cast<uchar>(audio[1]) & 0xe0) == 0xe0) return "mp3";
    return {};
}

QByteArray pcmToWave(const QByteArray &pcm, int sampleRate, int channels)
{
    QByteArray result(44, '\0');
    auto put16 = [&result](int offset, quint16 value) { qToLittleEndian(value, reinterpret_cast<uchar *>(result.data() + offset)); };
    auto put32 = [&result](int offset, quint32 value) { qToLittleEndian(value, reinterpret_cast<uchar *>(result.data() + offset)); };
    result.replace(0, 4, "RIFF"); put32(4, static_cast<quint32>(pcm.size()) + 36);
    result.replace(8, 8, "WAVEfmt "); put32(16, 16); put16(20, 1); put16(22, channels);
    put32(24, sampleRate); put32(28, sampleRate * channels * 2); put16(32, channels * 2); put16(34, 16);
    result.replace(36, 4, "data"); put32(40, static_cast<quint32>(pcm.size()));
    result += pcm;
    return result;
}

QJsonArray namedItems(const QJsonValue &root, const QString &key, const QString &provider = {})
{
    QJsonArray source;
    if (root.isArray()) source = root.toArray();
    else {
        auto o = root.toObject();
        if (o.value(key).isArray()) source = o.value(key).toArray();
        else if (o.value("data").isArray()) source = o.value("data").toArray();
    }
    QJsonArray result;
    for (const auto &entry : source) {
        QString id, name;
        QJsonArray voices;
        if (entry.isString()) id = name = entry.toString();
        else if (entry.isObject()) {
            const auto item = entry.toObject();
            id = item.value("id").toString(item.value("model_id").toString(item.value("voice_id").toString(item.value("name").toString())));
            if (key == "models") {
                if (provider == "elevenlabs" && !item.value("can_do_text_to_speech").toBool()) continue;
                if (provider == "openrouter") {
                    const auto modalities = item.value("architecture").toObject().value("output_modalities").toArray();
                    const auto alternate = item.value("output_modalities").toArray();
                    if (!modalities.contains("speech") && !alternate.contains("speech")) continue;
                    voices = namedItems(item.value("supported_voices"), "voices");
                }
                if (provider == "openai" && !id.startsWith("tts-") && !id.startsWith("gpt-4o-mini-tts")) continue;
            }
            name = item.value("name").toString(id);
        }
        if (!id.isEmpty()) {
            QJsonObject named{{"id", id}, {"name", name}};
            if (!voices.isEmpty()) named["voices"] = voices;
            result.append(named);
        }
    }
    return result;
}

QJsonObject resolveSchema(const QJsonObject &document, QJsonObject schema, int depth = 0)
{
    if (depth > 12) return {};
    const auto ref = schema.value("$ref").toString();
    if (ref.startsWith("#/")) {
        QJsonValue value = document;
        for (auto part : ref.mid(2).split('/')) {
            part.replace("~1", "/"); part.replace("~0", "~");
            value = value.toObject().value(part);
        }
        return resolveSchema(document, value.toObject(), depth + 1);
    }
    return schema;
}
}

TtsClient::TtsClient(QObject *parent) : QObject(parent), m_network(this)
{
    qRegisterMetaType<QVector<WordCue>>();
}

TtsClient::~TtsClient()
{
    m_destroying = true;
    cancel();
}

void TtsClient::setProfile(const QJsonObject &profile)
{
    cancel();
    m_profile = profile;
    m_detectedAdapter.clear();
    const auto discovery = profile.value("discovery").toObject();
    if (profile.value("adapter").toString("auto") == "auto"
        && discovery.value("speechEndpointKnown").toBool()
        && profile.value("discoveryBaseUrl").toString() == profile.value("baseUrl").toString()
        && profile.value("discoverySpeechPath").toString() == profile.value("speechPath").toString("/v1/audio/speech")) {
        const auto detected = discovery.value("suggestedAdapter").toString();
        if (detected == "openai" || detected == "vllm" || detected == "kokoro")
            m_detectedAdapter = detected;
    }
}

QJsonObject TtsClient::profile() const { return m_profile; }

void TtsClient::cancel()
{
    ++m_generation;
    const auto replies = m_pending.values();
    m_pending.clear();
    for (auto *reply : replies) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

QUrl TtsClient::baseUrl(QString *failure) const
{
    const QUrl url(m_profile.value("baseUrl").toString().trimmed(), QUrl::StrictMode);
    QString problem;
    if (!url.isValid() || url.host().isEmpty()) problem = tr("Enter a valid server address, including http:// or https://.");
    else if (url.scheme() != "http" && url.scheme() != "https") problem = tr("This adapter supports HTTP and HTTPS. Other protocols need a dedicated adapter.");
    else if (!url.userInfo().isEmpty()) problem = tr("Remove credentials from the server URL and use the API key field instead.");
    else if (url.hasFragment() || url.hasQuery()) problem = tr("Use a server address without a query string or fragment; put request options in the profile.");
    if (!problem.isEmpty()) { if (failure) *failure = problem; return {}; }
    return url;
}

QUrl TtsClient::endpoint(const QString &path, QString *failure) const
{
    QUrl base = baseUrl(failure);
    if (base.isEmpty()) return {};
    if (!base.path().endsWith('/')) base.setPath(base.path() + '/');
    const QUrl url = base.resolved(QUrl(path, QUrl::StrictMode));
    if (!url.isValid() || !sameOrigin(base, url) || !url.userInfo().isEmpty() || url.hasFragment()) {
        if (failure) *failure = tr("Endpoint paths must remain on the configured server and use HTTP or HTTPS.");
        return {};
    }
    return url;
}

void TtsClient::request(const QUrl &url, const QByteArray &body, bool get,
                        quint64 generation, Completion completion, int timeoutOverride)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept", get ? "application/json" : "audio/*, application/json, text/event-stream");
    request.setRawHeader("User-Agent", "SpeechDesk/1.0.0");
    if (!get) request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    const auto key = m_profile.value("apiKey").toString().trimmed();
    const bool elevenlabs = m_profile.value("adapter").toString() == "elevenlabs";
    if (!key.isEmpty()) {
        if (elevenlabs) request.setRawHeader("xi-api-key", key.toUtf8());
        else request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    }
    const auto extraHeaders = m_profile.value("headers").toObject();
    static const QRegularExpression headerName("^[!#$%&'*+.^_`|~0-9A-Za-z-]+$");
    for (auto it = extraHeaders.begin(); it != extraHeaders.end(); ++it) {
        const auto name = it.key().toUtf8();
        const auto value = it.value().toString().toUtf8();
        const auto lower = name.toLower();
        if (!headerName.match(it.key()).hasMatch() || value.contains('\r') || value.contains('\n')
            || lower == "host" || lower == "content-length" || lower == "connection"
            || lower == "transfer-encoding"
            || (!key.isEmpty() && (lower == "authorization" || lower == "xi-api-key"))) continue;
        request.setRawHeader(name, value);
    }
    auto *reply = get ? m_network.get(request) : m_network.post(request, body);
    m_pending.insert(reply);
    auto accumulated = QSharedPointer<QByteArray>::create();
    auto failure = QSharedPointer<QString>::create();
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    const int timeout = std::clamp(timeoutOverride > 0 ? timeoutOverride : m_profile.value("timeoutSeconds").toInt(180), 1, 1800);
    timer->start(timeout * 1000);
    connect(timer, &QTimer::timeout, this, [reply, failure] {
        *failure = tr("The speech server timed out. Check the connection or increase the profile timeout.");
        reply->abort();
    });
    connect(reply, &QIODevice::readyRead, this, [reply, accumulated, failure] {
        const auto bytes = reply->readAll();
        if (accumulated->size() + bytes.size() > MaxResponseBytes) {
            *failure = tr("The server response exceeded the 64 MiB limit. Use shorter text sections.");
            reply->abort();
        } else accumulated->append(bytes);
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, accumulated, failure, generation, completion = std::move(completion), timer] {
        timer->stop();
        m_pending.remove(reply);
        if (generation != m_generation || m_destroying) { reply->deleteLater(); return; }
        Response response;
        response.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.contentType = reply->rawHeader("Content-Type").toLower();
        response.sampleRate = reply->rawHeader("X-Audio-Sample-Rate").toInt();
        response.channels = reply->rawHeader("X-Audio-Channels").toInt();
        const auto last = reply->isOpen() ? reply->readAll() : QByteArray();
        if (accumulated->size() + last.size() <= MaxResponseBytes) accumulated->append(last);
        else *failure = tr("The server response exceeded the 64 MiB limit. Use shorter text sections.");
        response.data = std::move(*accumulated);
        if (!failure->isEmpty()) response.failure = *failure;
        else if (response.httpStatus >= 300 && response.httpStatus < 400)
            response.failure = tr("The server redirected this request. Configure the final server address; redirects are not followed with credentials.");
        else if (response.httpStatus == 401 || response.httpStatus == 403)
            response.failure = tr("The server rejected authentication. Check the profile's API key and access permissions.");
        else if (response.httpStatus == 402)
            response.failure = tr("The speech provider requires available credit or a suitable plan. Check your provider account.");
        else if (response.httpStatus == 404)
            response.failure = tr("The configured endpoint was not found. Check the adapter and endpoint path.");
        else if (response.httpStatus == 429)
            response.failure = tr("The server is busy or its request limit was reached. Try again shortly.");
        else if (response.httpStatus >= 400)
            response.failure = tr("The server rejected the request (HTTP %1). Check model, voice and supported options.").arg(response.httpStatus);
        else if (reply->error() != QNetworkReply::NoError)
            response.failure = tr("Could not connect to the speech server. Check its address, availability and TLS certificate.");
        else if (response.httpStatus < 200 || response.httpStatus >= 300)
            response.failure = tr("The server returned an unexpected HTTP response.");
        reply->deleteLater();
        completion(std::move(response));
    });
}

void TtsClient::probe()
{
    cancel();
    const auto generation = m_generation;
    QString failure;
    if (baseUrl(&failure).isEmpty()) { emit error(failure); return; }
    emit status(tr("Checking server capabilities…"));
    const auto adapter = m_profile.value("adapter").toString("auto");
    const QString provider = adapter == "openrouter" || adapter == "elevenlabs"
        ? adapter : m_profile.value("provider").toString();
    const bool hosted = provider == "openrouter" || provider == "elevenlabs" || provider == "openai";
    struct ProbeState { int pending = 3; bool reachable = false; QJsonObject api; QJsonArray models, voices, messages, evidence; };
    auto state = QSharedPointer<ProbeState>::create();
    const QStringList paths = {
        hosted ? QString() : m_profile.value("openApiPath").toString("/openapi.json"),
        m_profile.value("modelsPath").toString(provider == "openrouter"
            ? "/api/v1/models?output_modalities=speech" : "/v1/models"),
        provider == "openrouter" || provider == "openai" ? QString()
            : m_profile.value("voicesPath").toString(provider == "elevenlabs" ? "/v2/voices" : "/v1/audio/voices")};
    auto finish = [this, state, generation, provider, hosted] {
        if (--state->pending || generation != m_generation) return;
        const auto paths = state->api.value("paths").toObject();
        QString speechPath = m_profile.value("speechPath").toString("/v1/audio/speech");
        if (!speechPath.startsWith('/')) speechPath.prepend('/');
        const bool speechKnown = paths.contains(speechPath);
        const auto operation = paths.value(speechPath).toObject().value("post").toObject();
        const auto requestBody = resolveSchema(state->api, operation.value("requestBody").toObject());
        const auto schema = resolveSchema(state->api, requestBody.value("content").toObject()
            .value("application/json").toObject().value("schema").toObject());
        const auto properties = schema.value("properties").toObject();
        const QString title = state->api.value("info").toObject().value("title").toString().toLower();
        QString suggestion = "openai";
        if (speechKnown && (title.contains("vllm") || properties.contains("task_type"))) suggestion = "vllm";
        else if (speechKnown && title.contains("kokoro")) suggestion = "kokoro";
        if (speechKnown) m_detectedAdapter = suggestion;
        if (provider == "openai") {
            for (const auto &voice : {"alloy", "ash", "ballad", "coral", "echo", "fable", "nova", "onyx", "sage", "shimmer", "verse", "marin", "cedar"})
                state->voices.append(QJsonObject{{"id", voice}, {"name", voice}});
            state->messages.append(tr("Built-in OpenAI voice choices are model-dependent; confirm compatibility with your selected model."));
        } else if (provider == "openrouter") {
            state->messages.append(tr("Choose a voice from the selected OpenRouter model's list, or enter a supported voice ID. Voice IDs are model-specific."));
        } else if (provider == "elevenlabs") {
            state->messages.append(tr("Voice discovery shows the first page available to your account. You can also enter a voice ID manually."));
        }
        QJsonObject info{{"reachable", state->reachable}, {"speechEndpointKnown", speechKnown},
            {"providerPreset", hosted},
            {"suggestedAdapter", speechKnown ? suggestion : QString()}, {"models", state->models},
            {"voices", state->voices}, {"supportsTimings", properties.contains("word_timestamps")},
            {"supportsInstructions", properties.contains("instructions")},
            {"supportsLanguage", properties.contains("language")},
            {"supportsStreaming", properties.contains("stream") || properties.contains("stream_format")},
            {"messages", state->messages}, {"evidence", state->evidence}};
        if (!speechKnown) {
            auto messages = info["messages"].toArray();
            messages.append(hosted
                ? tr("The provider's documented speech endpoint is configured. A successful synthesis request is still required to verify your key, model and voice.")
                : tr("Speech support is unconfirmed. Discovery endpoints are optional; a manual profile and a speech test may still work."));
            info["messages"] = messages;
        }
        emit status(speechKnown ? tr("Speech endpoint detected. Review the voice and model settings.")
                               : state->reachable ? tr("Server reached; speech capabilities need manual selection.")
                                                  : tr("Discovery unavailable. Check the address or configure the profile manually."));
        emit capabilities(info);
        emit probeFinished(info);
    };
    for (int i = 0; i < paths.size(); ++i) {
        if (paths[i].isEmpty()) { finish(); continue; }
        const auto url = endpoint(paths[i], &failure);
        if (url.isEmpty()) { state->messages.append(failure); finish(); continue; }
        request(url, {}, true, generation, [state, i, finish, provider](Response response) {
            if (response.httpStatus > 0) state->reachable = true;
            if (response.failure.isEmpty()) {
                QJsonParseError parse;
                const auto document = QJsonDocument::fromJson(response.data, &parse);
                if (parse.error == QJsonParseError::NoError) {
                    QJsonValue value = document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
                    if (i == 0) state->api = document.object();
                    if (i == 1) state->models = namedItems(value, "models", provider);
                    if (i == 2) state->voices = namedItems(value, "voices");
                    state->evidence.append(i == 0 ? "OpenAPI document" : i == 1 ? "Model list" : "Voice list");
                }
            } else state->messages.append((i == 0 ? tr("API discovery: ") : i == 1 ? tr("Model discovery: ") : tr("Voice discovery: ")) + response.failure);
            finish();
        }, std::min(10, std::max(1, m_profile.value("timeoutSeconds").toInt(180))));
    }
}

void TtsClient::synthesize(const QString &text)
{
    cancel();
    const auto generation = m_generation;
    if (text.trimmed().isEmpty()) { emit error(tr("Enter some text to read.")); return; }
    QString failure;
    auto adapter = m_profile.value("adapter").toString("auto");
    if (adapter == "auto") adapter = m_detectedAdapter.isEmpty() ? "openai" : m_detectedAdapter;
    if (adapter != "openai" && adapter != "openrouter" && adapter != "elevenlabs"
        && adapter != "vllm" && adapter != "kokoro" && adapter != "custom") {
        emit error(tr("This profile uses an unsupported speech adapter. Select a provider or a compatible self-hosted connection.")); return;
    }
    QString speechPath = m_profile.value("speechPath").toString(adapter == "openrouter"
        ? "/api/v1/audio/speech" : adapter == "elevenlabs" ? "/v1/text-to-speech/${voice}" : "/v1/audio/speech");
    if (adapter == "elevenlabs") {
        const auto voice = m_profile.value("voice").toString().trimmed();
        if (voice.isEmpty() || voice == "." || voice == "..") {
            emit error(tr("Choose an ElevenLabs voice or enter its voice ID before generating speech.")); return;
        }
        const auto encodedVoice = QString::fromLatin1(QUrl::toPercentEncoding(voice));
        speechPath.replace("${voice}", encodedVoice).replace("{voice_id}", encodedVoice);
    }
    auto url = endpoint(speechPath, &failure);
    if (url.isEmpty()) { emit error(failure); return; }
    if (adapter == "elevenlabs") {
        QUrlQuery query(url);
        query.removeAllQueryItems("output_format");
        query.addQueryItem("output_format", m_profile.value("elevenlabsOutputFormat").toString("mp3_44100_128"));
        url.setQuery(query);
    }
    QJsonObject body;
    QJsonValue customTemplate = m_profile.value("requestTemplate");
    if (customTemplate.isString() && !customTemplate.toString().trimmed().isEmpty()) {
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(customTemplate.toString().toUtf8(), &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
            emit error(tr("The custom request template must be a JSON object. Put placeholders such as ${text} inside JSON strings.")); return;
        }
        customTemplate = doc.object();
    }
    if (customTemplate.isObject()) {
        QHash<QString, QString> variables{{"text", text}};
        for (const QString &key : {QStringLiteral("model"), QStringLiteral("voice"), QStringLiteral("language"), QStringLiteral("instructions")})
            variables[key] = m_profile.value(key).toString();
        body = substitute(customTemplate, variables).toObject();
    } else if (adapter == "elevenlabs") {
        body["text"] = text;
        if (!m_profile.value("model").toString().trimmed().isEmpty()) body["model_id"] = m_profile.value("model").toString();
        if (m_profile.value("sendLanguage").toBool() && !m_profile.value("language").toString().isEmpty())
            body["language_code"] = m_profile.value("language");
    } else {
        body["input"] = text;
        for (const QString &key : {QStringLiteral("model"), QStringLiteral("voice")})
            if (!m_profile.value(key).toString().trimmed().isEmpty()) body[key] = m_profile.value(key).toString();
        body["response_format"] = m_profile.value("responseFormat").toString(adapter == "openrouter" ? "mp3" : "wav");
        if (adapter == "vllm" || m_profile.value("sendLanguage").toBool())
            if (!m_profile.value("language").toString().isEmpty()) body["language"] = m_profile.value("language");
        if (adapter == "vllm" || m_profile.value("sendInstructions").toBool())
            if (!m_profile.value("instructions").toString().isEmpty()) body["instructions"] = m_profile.value("instructions");
    }
    const auto extras = m_profile.value("extraBody").toObject();
    for (auto it = extras.begin(); it != extras.end(); ++it) body[it.key()] = it.value();
    const auto encoded = QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (encoded.size() > MaxRequestBytes) { emit error(tr("The speech request is too large. Use shorter text sections.")); return; }
    emit status(tr("Generating speech…"));
    request(url, encoded, false, generation, [this, text, generation](Response response) {
        consume(std::move(response), text, generation);
    });
}

void TtsClient::consume(Response response, const QString &text, quint64 generation,
                        QVector<WordCue> inheritedCues, bool isAudioDownload)
{
    if (generation != m_generation) return;
    if (!response.failure.isEmpty()) { emit error(response.failure); return; }
    if (response.data.isEmpty()) { emit error(tr("The server returned an empty audio response.")); return; }
    const auto mode = isAudioDownload ? QStringLiteral("binary") : m_profile.value("responseMode").toString("auto");
    const auto trimmed = response.data.trimmed();
    if (mode == "sse" || (mode == "auto" && response.contentType.contains("text/event-stream"))) {
        consumeSse(response.data, text, generation); return;
    }
    if (mode == "json" || (mode == "auto" && (response.contentType.contains("json") || trimmed.startsWith('{')))) {
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(response.data, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) {
            emit error(tr("The server returned invalid JSON instead of audio.")); return;
        }
        consumeJson(document.object(), text, generation); return;
    }
    QString format = normalFormat(QString::fromLatin1(response.contentType));
    if (format.isEmpty() || format == "application/octet-stream") format = m_profile.value("responseFormat").toString("wav");
    deliverAudio(std::move(response.data), format, std::move(inheritedCues), response.sampleRate, response.channels);
}

void TtsClient::consumeJson(const QJsonObject &object, const QString &text, quint64 generation)
{
    if (object.contains("error")) { emit error(tr("The speech service returned an error. Check the model, voice and request options.")); return; }
    auto cues = readCues(object, m_profile.value("timingsField").toString("words"), text);
    auto field = atPath(object, m_profile.value("audioField").toString("audio"));
    QString encoding = m_profile.value("audioEncoding").toString("base64");
    if (!field.isString()) {
        for (const QString &candidate : {QStringLiteral("audio_base64"), QStringLiteral("data.audio"), QStringLiteral("audio_url"), QStringLiteral("url")}) {
            const auto value = atPath(object, candidate);
            if (value.isString()) { field = value; if (candidate.endsWith("url")) encoding = "url"; break; }
        }
    }
    if (!field.isString() || field.toString().isEmpty()) {
        emit error(tr("JSON response has no usable audio field. Configure the audio field and encoding in the profile.")); return;
    }
    QString payload = field.toString();
    if (encoding == "url") {
        QString failure;
        const QUrl url = endpoint(payload, &failure);
        if (url.isEmpty()) { emit error(tr("The returned audio URL must remain on the configured server.")); return; }
        request(url, {}, true, generation, [this, text, generation, cues](Response response) {
            consume(std::move(response), text, generation, cues, true);
        });
        return;
    }
    QString format = object.value("format").toString(object.value("response_format").toString(m_profile.value("responseFormat").toString("wav")));
    if (payload.startsWith("data:")) {
        const auto comma = payload.indexOf(',');
        if (comma < 0 || !payload.left(comma).endsWith(";base64")) { emit error(tr("The audio data URL must contain base64 audio.")); return; }
        format = payload.mid(5, comma - 5).section(';', 0, 0);
        payload = payload.mid(comma + 1);
    }
    const auto decoded = QByteArray::fromBase64Encoding(payload.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) { emit error(tr("The server returned invalid base64 audio.")); return; }
    deliverAudio(decoded.decoded, format, cues, object.value("sample_rate").toInt(), object.value("channels").toInt());
}

void TtsClient::consumeSse(const QByteArray &bytes, const QString &text, quint64 generation)
{
    Q_UNUSED(generation)
    QByteArray audio, eventData;
    QString format = m_profile.value("responseFormat").toString("pcm");
    int sampleRate = 0, channels = 0;
    QVector<WordCue> cues;
    bool bad = false;
    const auto flush = [&] {
        if (eventData.isEmpty()) return;
        const auto trimmed = eventData.trimmed();
        eventData.clear();
        if (trimmed == "[DONE]") return;
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(trimmed, &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject()) { bad = true; return; }
        const auto event = doc.object();
        const auto type = event.value("type").toString();
        if (type.contains("error") || event.contains("error")) { bad = true; return; }
        const auto payload = event.value("audio").toString(event.value("delta").toString());
        if (!payload.isEmpty()) {
            const auto decoded = QByteArray::fromBase64Encoding(payload.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded || audio.size() + decoded.decoded.size() > MaxResponseBytes) { bad = true; return; }
            audio.append(decoded.decoded);
        }
        if (event.contains("response_format")) format = event.value("response_format").toString();
        if (event.contains("sample_rate")) sampleRate = event.value("sample_rate").toInt();
        if (event.contains("channels")) channels = event.value("channels").toInt();
        // Per-delta timestamps may have a chunk-local time origin. Only consume
        // complete-clip timings in the terminal event, never guess offsets.
        if (type == "speech.audio.done") {
            const auto eventCues = readCues(event, m_profile.value("timingsField").toString("words"), text);
            if (!eventCues.isEmpty()) cues = eventCues;
        }
    };
    for (auto line : bytes.split('\n')) {
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) { flush(); if (bad) break; }
        else if (line.startsWith("data:")) {
            auto value = line.mid(5); if (value.startsWith(' ')) value.remove(0, 1);
            if (!eventData.isEmpty()) eventData += '\n';
            eventData += value;
        }
    }
    flush();
    if (bad) { emit error(tr("The speech event stream contained an error or invalid audio data.")); return; }
    deliverAudio(std::move(audio), format, cues, sampleRate, channels);
}

void TtsClient::deliverAudio(QByteArray audio, QString format, QVector<WordCue> cues, int sampleRate, int channels)
{
    if (audio.isEmpty() || audio.size() > MaxResponseBytes) { emit error(tr("The server returned no playable audio.")); return; }
    format = normalFormat(format);
    // PCM samples can accidentally resemble MP3 sync bytes. Explicit PCM has
    // priority; a genuine RIFF/WAVE header is still recognized for adapters
    // which return a WAV wrapper for a PCM request.
    const auto detected = format == "pcm" && !(audio.startsWith("RIFF") && audio.mid(8, 4) == "WAVE")
        ? QString() : sniffFormat(audio);
    if (!detected.isEmpty()) format = detected;
    if (format == "pcm") {
        if (!sampleRate) sampleRate = m_profile.value("sampleRate").toInt(24000);
        if (!channels) channels = m_profile.value("channels").toInt(1);
        if (sampleRate < 8000 || sampleRate > 192000 || channels < 1 || channels > 8 || audio.size() % (channels * 2)) {
            emit error(tr("Invalid PCM audio format. Set the correct 16-bit little-endian sample rate and channel count in the profile.")); return;
        }
        audio = pcmToWave(audio, sampleRate, channels);
        format = "wav";
    } else if (detected.isEmpty()) {
        emit error(tr("The server response is not recognized as WAV, MP3, FLAC or Ogg audio. Check the response adapter and format.")); return;
    }
    emit audioReady(std::move(audio), format, std::move(cues));
}

QVector<WordCue> TtsClient::readCues(const QJsonObject &object, const QString &path, const QString &text)
{
    const auto array = atPath(object, path).toArray();
    QVector<WordCue> cues;
    qsizetype cursor = 0;
    qint64 previous = -1;
    for (const auto &item : array) {
        const auto word = item.toObject();
        const QString label = word.value("word").toString(word.value("text").toString()).trimmed();
        if (label.isEmpty()) continue;
        const bool milliseconds = word.contains("start_ms") && word.contains("end_ms");
        const auto beginValue = word.value(milliseconds ? "start_ms" : "start");
        const auto endValue = word.value(milliseconds ? "end_ms" : "end");
        if (!beginValue.isDouble() || !endValue.isDouble()) continue;
        const double start = beginValue.toDouble(-1) * (milliseconds ? 1 : 1000);
        const double end = endValue.toDouble(-1) * (milliseconds ? 1 : 1000);
        if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end <= start || end > 24 * 3600000.0) continue;
        if (static_cast<qint64>(start) < previous) continue;
        const qsizetype found = text.indexOf(label, cursor, Qt::CaseInsensitive);
        if (found < 0 || found > std::numeric_limits<int>::max()) continue;
        cues.append({static_cast<int>(found), static_cast<int>(label.size()), static_cast<qint64>(std::llround(start)), static_cast<qint64>(std::llround(end))});
        cursor = found + label.size();
        previous = static_cast<qint64>(start);
    }
    return cues;
}

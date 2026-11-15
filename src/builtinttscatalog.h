#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

// Reader's bundled English preset catalog. No runtime discovery or downloads.
inline QStringList builtinTtsVoiceIds(const QString &model)
{
    if(model=="pocket")return {"alba","marius","javert","anna","charles","vera"};
    if(model=="kokoro")return {"af_heart","af_alloy","af_aoede","af_bella","af_jessica","af_kore","af_nicole","af_nova","af_river","af_sarah","af_sky","am_adam","am_echo","am_eric","am_fenrir","am_liam","am_michael","am_onyx","am_puck","am_santa","bf_alice","bf_emma","bf_isabella","bf_lily","bm_daniel","bm_fable","bm_george","bm_lewis"};
    return {};
}
inline QJsonObject builtinTtsCapabilities(const QString &model)
{
    QJsonArray models,voices;
    for(const auto &id:QStringList{"pocket","kokoro"}) {
        QJsonArray presets;
        for(const auto &voice:builtinTtsVoiceIds(id))presets.append(QJsonObject{{"id",voice},{"name",voice}});
        models.append(QJsonObject{{"id",id},{"name",id=="pocket"?"Pocket TTS":"Kokoro"},{"voices",presets}});
        if(id==model)voices=presets;
    }
    return {{"reachable",true},{"providerPreset",true},{"speechEndpointKnown",true},
        {"suggestedAdapter","builtin"},{"models",models},{"voices",voices},{"supportsTimings",false}};
}

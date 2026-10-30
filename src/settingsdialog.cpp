#include "settingsdialog.h"
#include "modelpacks.h"
#include "credentialdialogs.h"
#include "providerlifecycle.h"
#include <QDir>
#include <QStandardPaths>
#include <QTabWidget>
#include <QBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QPlainTextEdit>
#include <QFileDialog>
#include <QSaveFile>
#include <QJsonDocument>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QThread>
#include <QUuid>
#include <QProgressBar>
#include <QScrollArea>
#include <QTimer>
#include <QUrl>
#include <functional>

namespace {
QString defaultCredentialDirectory()
{
    QString config=qEnvironmentVariable("SPEECHDESK_CONFIG_DIR");
    if(config.isEmpty()) {
#ifdef Q_OS_WIN
        config=QDir(qEnvironmentVariable("APPDATA",QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))).filePath("Speech-Desk");
#else
        config=QDir(qEnvironmentVariable("XDG_CONFIG_HOME",QDir::home().filePath(".config"))).filePath("Speech-Desk");
#endif
    }
    return QDir(config).filePath("credentials");
}
class ModelImportDialog final:public QDialog {
public:
    using QDialog::QDialog;
    std::function<void()> requestCancellation;
    void reject() override {if(requestCancellation)requestCancellation();}
};
}

QJsonObject defaultServerProfile()
{
    return {{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"name","My speech server"}, {"baseUrl",""}, {"provider","selfhosted"},
        {"adapter","auto"},{"apiKey",""},{"model",""},{"voice",""},{"responseFormat","wav"},
        {"speechPath","/v1/audio/speech"},{"modelsPath","/v1/models"},{"voicesPath","/v1/audio/voices"},
        {"timeoutSeconds",180},{"maxChars",500},{"responseMode","auto"},{"audioField","audio"},
        {"audioEncoding","base64"},{"timingsField","words"},{"sampleRate",24000},{"channels",1}};
}
QJsonObject serverProfileForProvider(const QString &provider)
{
    auto p=defaultServerProfile();
    if(provider=="openai") {
        p["provider"]="openai";p["name"]="OpenAI";p["baseUrl"]="https://api.openai.com";
        p["adapter"]="openai";p["model"]="gpt-realtime-2.1-mini";p["voice"]="marin";p["realtimePath"]="/v1/realtime";
        p["voicesPath"]="";p["responseFormat"]="wav";
    } else if(provider=="elevenlabs") {
        p["provider"]="elevenlabs";p["name"]="ElevenLabs";p["baseUrl"]="https://api.elevenlabs.io";
        p["adapter"]="elevenlabs";p["model"]="eleven_multilingual_v2";
        p["speechPath"]="/v1/text-to-speech/${voice}";p["voicesPath"]="/v2/voices";
        p["responseFormat"]="mp3";p["elevenlabsOutputFormat"]="mp3_44100_128";
    } else if(provider=="openrouter") {
        p["provider"]="openrouter";p["name"]="OpenRouter";p["baseUrl"]="https://openrouter.ai";
        p["adapter"]="openrouter";p["model"]="mistralai/voxtral-mini-tts-2603";p["voice"]="en_paul_neutral";
        p["speechPath"]="/api/v1/audio/speech";p["modelsPath"]="/api/v1/models?output_modalities=speech";
        p["voicesPath"]="";p["responseFormat"]="mp3";
    }
    return p;
}
QJsonObject currentServerProfile(QJsonObject p)
{
    if(!ProviderLifecycle::modelAllowed(p,p.value("model").toString())){p["model"]="";p["modelRetired"]=true;}
    if(p.contains("discovery")) {
        auto discovery=p.value("discovery").toObject();discovery["models"]=ProviderLifecycle::modelChoices(p,discovery.value("models").toArray());p["discovery"]=discovery;
    }
    return p;
}
QJsonObject publicServerProfile(QJsonObject p)
{
    for(const auto &key:QStringList{"apiKey","credentialId","credentialRef","credentialFile","credentialPath","credentialSaved","saveApiKey","modelRetired"})p.remove(key);
    QJsonObject headers=p.value("headers").toObject();
    for(const auto &key:headers.keys())if(QStringList{"authorization","proxy-authorization","x-api-key","api-key","xi-api-key","cookie"}.contains(key.toLower()))headers.remove(key);
    if(!headers.isEmpty())p["headers"]=headers;else p.remove("headers");
    return p;
}
SettingsDialog::SettingsDialog(const QJsonObject &settings,QWidget *parent,const QString &credentialDirectory):QDialog(parent),m_settings(settings),m_credentialDirectory(credentialDirectory.isEmpty()?defaultCredentialDirectory():credentialDirectory),m_credentials(m_credentialDirectory),m_probe(this)
{
    // Profiles have their own working copy. Do not retain another unlocked-key
    // copy in the preferences snapshot after an explicit Lock now action.
    m_settings.remove("profiles");
    setWindowTitle("Settings — SpeechDesk");resize(780,650);setMinimumSize(620,510);
    auto outer=new QVBoxLayout(this);outer->setContentsMargins(20,20,20,16);outer->setSpacing(14);
    m_tabs=new QTabWidget;outer->addWidget(m_tabs);
    auto appearance=new QWidget;auto a=new QFormLayout(appearance);a->setContentsMargins(20,24,20,20);a->setSpacing(18);a->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_theme=new QComboBox;m_theme->addItem("Follow system","system");m_theme->addItem("Light","light");m_theme->addItem("Dark","dark");
    m_theme->setCurrentIndex(qMax(0,m_theme->findData(settings.value("theme").toString("system"))));a->addRow("Theme",m_theme);
    m_fontSize=new QSpinBox;m_fontSize->setRange(11,28);m_fontSize->setValue(settings.value("fontSize").toInt(16));m_fontSize->setSuffix(" px");a->addRow("Editor text size",m_fontSize);
    m_highlight=new QCheckBox("Highlight the text being read");m_highlight->setChecked(settings.value("highlight").toBool(true));a->addRow(m_highlight);
    m_follow=new QCheckBox("Keep the current sentence in view");m_follow->setChecked(settings.value("follow").toBool(true));a->addRow(m_follow);
    auto detail=new QLabel("Word highlighting follows server timing data. When timings are unavailable, the current sentence is highlighted for the duration of its audio.");detail->setWordWrap(true);detail->setProperty("muted",true);a->addRow(detail);
    m_tabs->addTab(appearance,"Appearance");

    auto servers=new QWidget;auto sv=new QVBoxLayout(servers);sv->setContentsMargins(20,16,20,12);sv->setSpacing(9);
    auto row=new QHBoxLayout;m_profilesBox=new QComboBox;m_profilesBox->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);row->addWidget(m_profilesBox,1);
    auto add=new QPushButton("Add");auto remove=new QPushButton("Remove");row->addWidget(add);row->addWidget(remove);sv->addLayout(row);
    auto form=new QFormLayout;form->setSpacing(7);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_provider=new QComboBox;m_provider->setObjectName("speechProvider");m_provider->addItem("Local / remote private server","selfhosted");m_provider->addItem("OpenAI","openai");m_provider->addItem("ElevenLabs","elevenlabs");m_provider->addItem("OpenRouter","openrouter");form->addRow("Provider",m_provider);
    m_name=new QLineEdit;m_name->setObjectName("profileName");form->addRow("Profile name",m_name);
    m_url=new QLineEdit;m_url->setObjectName("providerAddress");m_url->setPlaceholderText("http://your-server:8000");form->addRow("Server address",m_url);
    m_adapter=new QComboBox;m_adapter->addItem("Automatic / compatible speech API","auto");m_adapter->addItem("OpenAI-compatible speech","openai");m_adapter->addItem("vLLM-Omni","vllm");m_adapter->addItem("Kokoro-FastAPI","kokoro");m_adapter->addItem("Custom HTTP / JSON / SSE","custom");m_adapter->addItem("ElevenLabs speech","elevenlabs");m_adapter->addItem("OpenRouter speech","openrouter");form->addRow("Connection type",m_adapter);
    m_key=new QLineEdit;m_key->setObjectName("providerApiKey");m_key->setEchoMode(QLineEdit::Password);m_key->setPlaceholderText("Kept for this session only");m_key->setToolTip("API keys stay in memory unless you explicitly save an encrypted key. Plaintext keys are never written to settings or exported profiles.");form->addRow("API key",m_key);
    auto keyOptions=new QWidget;auto keyLayout=new QVBoxLayout(keyOptions);keyLayout->setContentsMargins(0,0,0,0);keyLayout->setSpacing(6);
    m_saveKey=new QCheckBox("Save API key encrypted");m_saveKey->setObjectName("saveEncryptedApiKey");keyLayout->addWidget(m_saveKey);
    auto keyActions=new QHBoxLayout;keyActions->setSpacing(6);m_unlockKey=new QPushButton("Unlock…");m_unlockKey->setObjectName("unlockApiKey");m_lockKey=new QPushButton("Lock now");m_lockKey->setObjectName("lockApiKey");m_replaceKey=new QPushButton("Replace saved key…");m_replaceKey->setObjectName("replaceApiKey");keyActions->addWidget(m_unlockKey);keyActions->addWidget(m_lockKey);keyActions->addWidget(m_replaceKey);keyActions->addStretch();keyLayout->addLayout(keyActions);
    m_credentialInfo=new QLabel;m_credentialInfo->setObjectName("credentialStatus");m_credentialInfo->setWordWrap(true);m_credentialInfo->setProperty("muted",true);keyLayout->addWidget(m_credentialInfo);form->addRow("Key protection",keyOptions);
    auto mv=new QWidget;auto ml=new QHBoxLayout(mv);ml->setContentsMargins(0,0,0,0);m_model=new QLineEdit;m_model->setObjectName("providerModel");m_model->setPlaceholderText("Model ID");m_voice=new QLineEdit;m_voice->setObjectName("providerVoice");m_voice->setPlaceholderText("Voice ID");ml->addWidget(m_model);ml->addWidget(m_voice);form->addRow("Model / voice",mv);
    auto limits=new QWidget;auto ll=new QHBoxLayout(limits);ll->setContentsMargins(0,0,0,0);m_timeout=new QSpinBox;m_timeout->setRange(10,1800);m_timeout->setSuffix(" s timeout");m_chunkSize=new QSpinBox;m_chunkSize->setRange(80,8000);m_chunkSize->setSuffix(" characters / section");ll->addWidget(m_timeout);ll->addWidget(m_chunkSize);form->addRow("Request limits",limits);
    sv->addLayout(form);
    m_providerInfo=new QLabel;m_providerInfo->setObjectName("providerNotice");m_providerInfo->setWordWrap(true);m_providerInfo->setProperty("muted",true);sv->addWidget(m_providerInfo);
    m_lifecycleInfo=new QLabel;m_lifecycleInfo->setObjectName("providerLifecycleNotice");m_lifecycleInfo->setWordWrap(true);m_lifecycleInfo->setProperty("muted",true);sv->addWidget(m_lifecycleInfo);
    auto controls=new QHBoxLayout;auto test=new QPushButton("Detect capabilities");test->setProperty("accent",true);controls->addWidget(test);auto advanced=new QPushButton("Advanced…");controls->addWidget(advanced);controls->addStretch();auto import=new QPushButton("Import…");auto exportButton=new QPushButton("Export…");controls->addWidget(import);controls->addWidget(exportButton);sv->addLayout(controls);
    m_connectionInfo=new QLabel("Enter a server address, then detect its models, voices and supported features. You can always set the connection type manually.");m_connectionInfo->setWordWrap(true);m_connectionInfo->setProperty("muted",true);m_connectionInfo->setMinimumHeight(36);sv->addWidget(m_connectionInfo);sv->addStretch();
    auto serversScroll=new QScrollArea;serversScroll->setFrameShape(QFrame::NoFrame);serversScroll->setWidgetResizable(true);serversScroll->setWidget(servers);m_tabs->addTab(serversScroll,"Servers");
    auto recognition=new QWidget;auto st=new QFormLayout(recognition);st->setContentsMargins(20,24,20,20);st->setSpacing(18);
    m_threads=new QSpinBox;m_threads->setRange(1,32);m_threads->setValue(settings.value("threads").toInt(qMin(4,qMax(1,QThread::idealThreadCount()))));st->addRow("Recognition CPU threads",m_threads);
    auto notice=new QLabel("Dictation adds words directly to the text you want to read aloud. Whistle and Whisper Tiny, Base and Small are included.\n\nWhistle: English, German, French, Spanish, Italian, Dutch and Polish. Whisper also supports Hungarian and other languages.\n\nLive text is provisional; Stop recording produces the final text. Cancel restores the text from before dictation. Recognition stays on this computer.");notice->setWordWrap(true);st->addRow(notice);
    auto packs=new QPushButton("Import model pack…");packs->setObjectName("importModelPackButton");st->addRow("Optional dictation models",packs);connect(packs,&QPushButton::clicked,this,&SettingsDialog::importModelPack);
    m_modelPackInfo=new QLabel;m_modelPackInfo->setObjectName("modelPackInfo");m_modelPackInfo->setWordWrap(true);m_modelPackInfo->setProperty("muted",true);st->addRow(m_modelPackInfo);refreshModelPacks();
    auto packNote=new QLabel("Import the complete ZIP for the model you downloaded. Installed models appear in Dictate and remain installed when this dialog closes. Larger models may work best with Live text off.");packNote->setWordWrap(true);packNote->setProperty("muted",true);st->addRow(packNote);m_tabs->addTab(recognition,"Dictation");
    auto about=new QWidget;about->setObjectName("aboutPage");auto ab=new QVBoxLayout(about);ab->setContentsMargins(24,24,24,24);auto title=new QLabel("SpeechDesk 1.1.1");title->setProperty("heading",true);ab->addWidget(title);auto description=new QLabel("Text to speech with OpenAI, ElevenLabs, OpenRouter or your own speech server.\n\nType, paste, open a text file or dictate into one editor. Generated speech plays inside SpeechDesk with passage highlighting.\n\nWhistle and Whisper provide local dictation. Optional larger models are available as separate ZIP add-ons.\n\nCopyright (c) 2026 davoda1. MIT License.\nThird-party components retain their own licenses.\n\nNo telemetry or automatic model downloads. Speech output is AI-generated.");description->setWordWrap(true);ab->addWidget(description);
    ab->addSpacing(16);
    auto manualRow=new QHBoxLayout;manualRow->setSpacing(12);
    auto manualHint=new QLabel("Open the manual website in your browser for\nsetup instructions and help using SpeechDesk.");manualHint->setObjectName("manualHint");manualHint->setProperty("muted",true);manualHint->setFixedHeight(36);manualHint->setAlignment(Qt::AlignLeft|Qt::AlignVCenter);manualRow->addWidget(manualHint,0,Qt::AlignVCenter);
    auto manual=new QPushButton("Manual");manual->setObjectName("manualButton");manual->setAutoDefault(false);manual->setMinimumSize(110,36);manual->setToolTip("Open the SpeechDesk manual in your browser");manualRow->addWidget(manual,0,Qt::AlignVCenter);manualRow->addStretch();ab->addLayout(manualRow);
    connect(manual,&QPushButton::clicked,this,[]{QDesktopServices::openUrl(QUrl("https://davoda1.github.io/Speech-Desk-Manual/"));});
    ab->addStretch();m_tabs->addTab(about,"About");
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);outer->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,this,&SettingsDialog::saveAndAccept);connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    m_profiles=settings.value("profiles").toArray();if(m_profiles.isEmpty())m_profiles.append(defaultServerProfile());refreshProfiles(qBound(0,settings.value("selectedProfile").toInt(),int(m_profiles.size())-1));
    connect(m_profilesBox,&QComboBox::currentIndexChanged,this,[this](int idx){storeProfile();loadProfile(idx);});
    connect(m_provider,&QComboBox::activated,this,&SettingsDialog::applyProvider);
    connect(m_model,&QLineEdit::textChanged,this,[this]{updateProviderInfo();});
    connect(m_key,&QLineEdit::textEdited,this,[this]{refreshCredentialControls();});
    connect(m_unlockKey,&QPushButton::clicked,this,[this]{unlockCurrentCredential();});
    connect(m_lockKey,&QPushButton::clicked,this,&SettingsDialog::lockCurrentCredential);
    connect(m_replaceKey,&QPushButton::clicked,this,[this]{protectCurrentCredential(true);});
    connect(m_saveKey,&QCheckBox::clicked,this,[this](bool checked){
        if(checked)protectCurrentCredential(false);
        else if(m_current>=0) {
            if(QMessageBox::question(this,"Remove encrypted key","Remove this profile's saved encrypted key when Settings are saved? An unlocked key remains available for this session.") == QMessageBox::Yes)stageCredentialRemoval(m_profiles[m_current].toObject().value("id").toString());
            refreshCredentialControls();
        }
    });
    connect(m_url,&QLineEdit::textEdited,this,[this]{
        m_probe.cancel();m_key->clear();
        if(m_current>=0&&m_current<m_profiles.size()) {
            auto p=publicServerProfile(m_profiles[m_current].toObject());stageCredentialRemoval(p.value("id").toString());
            p.remove("discovery");p.remove("discoveryBaseUrl");p.remove("discoverySpeechPath");m_profiles[m_current]=p;refreshCredentialControls();updateProviderInfo();
        }
    });
    connect(add,&QPushButton::clicked,this,[this]{storeProfile();auto p=defaultServerProfile();p["name"]="New speech profile";m_profiles.append(p);refreshProfiles(m_profiles.size()-1);});
    connect(remove,&QPushButton::clicked,this,[this]{if(m_profiles.size()<=1){QMessageBox::information(this,"Server profiles","Keep at least one profile. You can clear its address to leave it unconfigured.");return;}if(QMessageBox::question(this,"Remove profile","Remove this saved connection profile?")!=QMessageBox::Yes)return;stageCredentialRemoval(m_profiles[m_current].toObject().value("id").toString());m_profiles.removeAt(m_current);m_current=-1;refreshProfiles(0);});
    connect(advanced,&QPushButton::clicked,this,&SettingsDialog::editAdvanced);connect(import,&QPushButton::clicked,this,&SettingsDialog::importProfile);connect(exportButton,&QPushButton::clicked,this,&SettingsDialog::exportProfile);connect(test,&QPushButton::clicked,this,&SettingsDialog::detect);
    auto lifecycleTimer=new QTimer(this);
    connect(lifecycleTimer,&QTimer::timeout,this,[this,lastDay=ProviderLifecycle::utcToday()]() mutable {
        const auto day=ProviderLifecycle::utcToday();if(day==lastDay)return;lastDay=day;
        if(m_current<0)return;auto profile=m_profiles[m_current].toObject();profile["baseUrl"]=m_url->text();profile["model"]=m_model->text();
        if(!ProviderLifecycle::modelAllowed(profile,m_model->text())) {
            m_probe.cancel();profile=currentServerProfile(profile);m_profiles[m_current]=profile;m_model->clear();m_model->setPlaceholderText("Retired model — choose replacement");
        }
        updateProviderInfo();
    });lifecycleTimer->start(60000);
    connect(&m_probe,&TtsClient::status,m_connectionInfo,&QLabel::setText);
    connect(&m_probe,&TtsClient::error,m_connectionInfo,&QLabel::setText);
    connect(&m_probe,&TtsClient::probeFinished,this,[this](const QJsonObject &info){
        if(m_current<0)return;auto p=m_profiles[m_current].toObject();p["discovery"]=info;p["discoveryBaseUrl"]=p["baseUrl"];p["discoverySpeechPath"]=p.value("speechPath").toString("/v1/audio/speech");m_profiles[m_current]=p;
        QString message=info.value("reachable").toBool()?"Server responded. ":"No compatible discovery response. ";
        message+=QString("%1 model(s), %2 voice(s). ").arg(info.value("models").toArray().size()).arg(info.value("voices").toArray().size());
        message+=info.value("speechEndpointKnown").toBool()?"Speech endpoint found in the server description.":"Speech generation still needs a successful request; manual configuration remains available.";
        m_connectionInfo->setText(message);m_connectionInfo->setToolTip(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Indented)));
        if(m_model->text().isEmpty()&&!info.value("models").toArray().isEmpty())m_model->setText(info.value("models").toArray().first().toObject().value("id").toString());
        if(m_voice->text().isEmpty()&&!info.value("voices").toArray().isEmpty())m_voice->setText(info.value("voices").toArray().first().toObject().value("id").toString());
    });
}
void SettingsDialog::showServers(){m_tabs->setCurrentIndex(1);}
void SettingsDialog::refreshProfiles(int index){QSignalBlocker b(m_profilesBox);m_profilesBox->clear();for(auto p:m_profiles)m_profilesBox->addItem(p.toObject().value("name").toString("Speech server"));m_profilesBox->setCurrentIndex(index);loadProfile(index);}
void SettingsDialog::loadProfile(int index)
{
    m_probe.cancel();m_current=index;
    if(index<0||index>=m_profiles.size())return;
    const auto p=currentServerProfile(m_profiles[index].toObject());m_profiles[index]=p;
    const QString provider=p.value("provider").toString("selfhosted");
    QSignalBlocker providerSignals(m_provider);
    m_provider->setCurrentIndex(qMax(0,m_provider->findData(provider)));
    m_name->setText(p.value("name").toString());
    m_url->setText(p.value("baseUrl").toString());
    m_key->setText(p.value("apiKey").toString());
    const bool modelAllowed=!p.value("modelRetired").toBool()&&ProviderLifecycle::modelAllowed(p,p.value("model").toString());
    m_model->setText(modelAllowed?p.value("model").toString():QString());m_model->setPlaceholderText(modelAllowed?"Model ID":"Retired model — choose replacement");m_voice->setText(p.value("voice").toString());
    m_adapter->setCurrentIndex(qMax(0,m_adapter->findData(p.value("adapter").toString("auto"))));
    m_timeout->setValue(p.value("timeoutSeconds").toInt(180));
    updateProviderInfo();
    m_chunkSize->setValue(p.value("maxChars").toInt(500));
    m_connectionInfo->setText("Detect models and voices, or enter their IDs. Detection does not generate speech.");
    m_connectionInfo->setToolTip({});refreshCredentialControls();
}
void SettingsDialog::storeProfile()
{
    if(m_current<0||m_current>=m_profiles.size())return;
    auto p=m_profiles[m_current].toObject();
    p["name"]=m_name->text().trimmed().isEmpty()?"Speech profile":m_name->text().trimmed();
    p["provider"]=m_provider->currentData().toString();
    p["baseUrl"]=m_url->text().trimmed();p["adapter"]=m_adapter->currentData().toString();
    p["apiKey"]=m_key->text();p["model"]=m_model->text().trimmed();p["voice"]=m_voice->text().trimmed();
    if(!p.value("model").toString().isEmpty()&&ProviderLifecycle::modelAllowed(p,p.value("model").toString()))p.remove("modelRetired");
    p["timeoutSeconds"]=m_timeout->value();p["maxChars"]=m_chunkSize->value();
    m_profiles[m_current]=p;m_profilesBox->setItemText(m_current,p["name"].toString());
}
void SettingsDialog::applyProvider(int index)
{
    if(m_current<0||m_current>=m_profiles.size())return;
    const QString provider=m_provider->itemData(index).toString();
    const auto previous=m_profiles[m_current].toObject();
    if(provider==previous.value("provider").toString("selfhosted"))return;
    m_probe.cancel();stageCredentialRemoval(previous.value("id").toString());
    // Build a fresh profile so keys, custom headers, request templates and
    // discovered metadata cannot cross from one provider to another.
    auto p=serverProfileForProvider(provider);
    p["id"]=previous.value("id");
    const QString name=m_name->text().trimmed();
    if(!name.isEmpty()&&!QStringList{"My speech server","New speech profile","OpenAI","ElevenLabs","OpenRouter"}.contains(name))p["name"]=name;
    m_profiles[m_current]=p;
    refreshProfiles(m_current);
    m_connectionInfo->setText("Provider preset applied. Enter this service's API key and review its model and voice before speaking.");
}
void SettingsDialog::updateProviderInfo()
{
    const QString provider=m_provider->currentData().toString();
    const bool hosted=provider!="selfhosted";
    m_adapter->setEnabled(!hosted);m_url->setReadOnly(hosted);
    m_chunkSize->setMaximum(provider=="openai"?4096:8000);

    m_url->setToolTip(hosted?"This preset uses the provider's service address. Use a private-server profile for another endpoint.":"Address of your speech service. Changing it clears the current API key.");
    m_model->setToolTip("Use a speech model ID offered by the selected service. A general chat model does not necessarily support speech.");
    m_voice->setToolTip(provider=="elevenlabs"?"Use Detect capabilities to find voices available to your ElevenLabs account.":"Voice IDs are specific to the selected model. Check the service's voice list when changing models.");
    auto profile=m_current>=0&&m_current<m_profiles.size()?m_profiles[m_current].toObject():QJsonObject{};
    profile["baseUrl"]=m_url->text();
    const QString storedModel=profile.value("model").toString();
    const QString displayedModel=m_model->text().isEmpty()&&!ProviderLifecycle::modelAllowed(profile,storedModel)?storedModel:m_model->text();
    profile["model"]=displayedModel;
    const bool hiddenRetired=profile.value("modelRetired").toBool()&&m_model->text().isEmpty();
    const bool legacy=hiddenRetired||(ProviderLifecycle::isOfficialOpenAi(profile)&&ProviderLifecycle::isLegacySpeechModel(displayedModel));
    m_lifecycleInfo->setVisible(legacy||ProviderLifecycle::usesRealtime(profile));
    m_lifecycleInfo->setText(legacy
        ?(!hiddenRetired&&ProviderLifecycle::modelAllowed(profile,displayedModel)?"This OpenAI Speech API model retires on 6 January 2027. Select gpt-realtime-2.1-mini to use OpenAI Realtime.":"This OpenAI Speech API model is retired and cannot be used. Select gpt-realtime-2.1-mini or another supported model.")
        :"OpenAI Realtime · SpeechDesk reads your text using the selected model and voice.");
    m_providerInfo->setText(hosted
        ?QString("Text is sent to %1 for AI-generated speech; API usage may incur charges. Dictation stays on this computer.").arg(m_provider->currentText())
        :"Text is sent to your configured service for AI-generated speech. Dictation stays on this computer.");
}
QJsonObject SettingsDialog::resultSettings(){storeProfile();auto s=m_settings;s["profiles"]=m_profiles;s["selectedProfile"]=m_current;s["theme"]=m_theme->currentData().toString();s["fontSize"]=m_fontSize->value();s["follow"]=m_follow->isChecked();s["highlight"]=m_highlight->isChecked();s["threads"]=m_threads->value();return s;}
void SettingsDialog::detect(){storeProfile();if(!unlockCurrentCredential())return;m_probe.setProfile(m_profiles[m_current].toObject());m_probe.probe();}
void SettingsDialog::setSaveHandler(std::function<bool(const QJsonObject &,QString *)> handler){m_saveHandler=std::move(handler);}
bool SettingsDialog::hasSavedCredential(const QString &id) const
{
    return !m_pendingCredentialRemovals.contains(id)&&(m_pendingCredentials.contains(id)||m_credentials.exists(id));
}
void SettingsDialog::stageCredentialRemoval(const QString &id)
{
    if(id.isEmpty())return;
    m_pendingCredentials.remove(id);
    if(m_credentials.exists(id))m_pendingCredentialRemovals.insert(id);
}
void SettingsDialog::refreshCredentialControls()
{
    if(m_current<0||m_current>=m_profiles.size())return;
    const QString id=m_profiles[m_current].toObject().value("id").toString();
    const bool saved=hasSavedCredential(id),unlocked=!m_key->text().trimmed().isEmpty();
    QSignalBlocker blocked(m_saveKey);m_saveKey->setChecked(saved);
    m_key->setReadOnly(saved);m_key->setPlaceholderText(saved?"Saved encrypted key is locked":(m_provider->currentData().toString()=="selfhosted"?"Optional — this session only":"Required — this session only"));
    m_unlockKey->setEnabled(saved&&!unlocked);m_lockKey->setEnabled(unlocked);m_replaceKey->setEnabled(saved);
    if(m_pendingCredentials.contains(id))m_credentialInfo->setText(unlocked?"Encrypted key ready · save Settings to store it. Unlocked for this session.":"Encrypted key ready · save Settings to store it. Locked.");
    else if(m_pendingCredentialRemovals.contains(id))m_credentialInfo->setText("Saved key will be removed when Settings are saved.");
    else if(saved)m_credentialInfo->setText(unlocked?"Saved encrypted key · unlocked for this session.":"Saved encrypted key · locked. Unlock when you want to use this profile.");
    else m_credentialInfo->setText(unlocked?"Session-only key · it will be forgotten when SpeechDesk closes.":"No saved API key. Your passphrase is never stored.");
}
bool SettingsDialog::unlockCurrentCredential()
{
    if(m_current<0||m_current>=m_profiles.size())return false;
    storeProfile();const auto profile=m_profiles[m_current].toObject();const auto id=profile.value("id").toString();
    if(!m_key->text().trimmed().isEmpty()||!hasSavedCredential(id))return true;
    QString key;
    if(!unlockCredentialForProfile(this,profile,m_credentialDirectory,&key,m_pendingCredentials.value(id)))return false;
    m_key->setText(key);key.fill(QChar(0));storeProfile();refreshCredentialControls();return true;
}
void SettingsDialog::protectCurrentCredential(bool replacement)
{
    if(m_current<0||m_current>=m_profiles.size())return;
    storeProfile();const auto profile=m_profiles[m_current].toObject();const auto id=profile.value("id").toString();
    if(CredentialStore::normalizedOrigin(profile.value("baseUrl").toString()).isEmpty()) {
        QMessageBox::information(this,"Server address needed","Enter the server address before saving an encrypted API key.");refreshCredentialControls();return;
    }
    QByteArray encrypted;QString key;
    if(protectCredentialForProfile(this,profile,m_key->text(),replacement,&encrypted,&key)) {
        m_pendingCredentials[id]=encrypted;m_pendingCredentialRemovals.remove(id);
        m_key->setText(key);key.fill(QChar(0));storeProfile();
    }
    refreshCredentialControls();
}
void SettingsDialog::lockCurrentCredential()
{
    if(m_current<0||m_current>=m_profiles.size())return;
    m_probe.setProfile({});const QString id=m_profiles[m_current].toObject().value("id").toString();
    m_key->clear();auto profile=publicServerProfile(m_profiles[m_current].toObject());m_profiles[m_current]=profile;
    // Lock is an immediate session action; it never changes the encrypted file.
    emit credentialLocked(id);refreshCredentialControls();
}
bool SettingsDialog::commitChanges(QString *error)
{
    storeProfile();const auto settings=resultSettings();
    for(const auto &entry:settings.value("profiles").toArray()) {
        const auto profile=entry.toObject();
        if(!ProviderLifecycle::modelAllowed(profile,profile.value("model").toString())) {
            if(error)*error=QString("The profile %1 uses a retired OpenAI model. Choose gpt-realtime-2.1-mini or another supported model before saving.").arg(profile.value("name").toString("Speech profile"));
            return false;
        }
    }
    QSet<QString> affected=m_pendingCredentialRemovals;
    for(auto it=m_pendingCredentials.cbegin();it!=m_pendingCredentials.cend();++it)affected.insert(it.key());
    QMap<QString,CredentialStore::Snapshot> before;
    for(const auto &id:affected) {
        CredentialStore::Snapshot state;if(!m_credentials.snapshot(id,&state,error))return false;
        before[id]=state;
    }
    QStringList changed;
    auto rollback=[&](const QString &failure){
        QStringList rollbackErrors;
        for(auto it=changed.crbegin();it!=changed.crend();++it) {
            QString detail;
            const bool ok=m_credentials.restoreSnapshot(*it,before.value(*it),&detail);
            if(!ok)rollbackErrors.append(detail);
        }
        if(error)*error=failure+(rollbackErrors.isEmpty()?QString():"\nSome saved keys could not be restored: "+rollbackErrors.join("; "));
        return false;
    };
    for(auto it=m_pendingCredentials.cbegin();it!=m_pendingCredentials.cend();++it) {
        QString detail;if(!m_credentials.commit(it.key(),it.value(),&detail))return rollback(detail);
        changed.append(it.key());
    }
    for(const auto &id:m_pendingCredentialRemovals) {
        QString detail;if(!m_credentials.remove(id,&detail))return rollback(detail);
        changed.append(id);
    }
    QString detail;
    if(m_saveHandler&&!m_saveHandler(settings,&detail))return rollback(detail.isEmpty()?"Could not save Settings.":detail);
    m_pendingCredentials.clear();m_pendingCredentialRemovals.clear();return true;
}
void SettingsDialog::saveAndAccept()
{
    QString error;
    if(!commitChanges(&error)){QMessageBox::warning(this,"Settings not saved",error+"\nYour changes remain in this dialog. Try again or cancel.");return;}
    accept();
}
void SettingsDialog::editAdvanced()
{
    storeProfile();QDialog d(this);d.setWindowTitle("Advanced server profile");d.resize(760,640);auto l=new QVBoxLayout(&d);
    const auto original=m_profiles[m_current].toObject();
    auto hint=new QLabel("Edit the connection profile as JSON. See the Server Profiles guide for custom requests and response mappings. API keys are managed in the masked field and Key protection controls. Custom request fields are saved; keep secrets out of them. Changing the service address or provider clears its current key. The profile identity cannot be changed here.");hint->setWordWrap(true);l->addWidget(hint);
    auto editor=new QPlainTextEdit(QString::fromUtf8(QJsonDocument(publicServerProfile(original)).toJson(QJsonDocument::Indented)));editor->setLineWrapMode(QPlainTextEdit::NoWrap);l->addWidget(editor);
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);l->addWidget(buttons);connect(buttons,&QDialogButtonBox::rejected,&d,&QDialog::reject);connect(buttons,&QDialogButtonBox::accepted,&d,[&]{
        QJsonParseError error;auto doc=QJsonDocument::fromJson(editor->toPlainText().toUtf8(),&error);if(error.error!=QJsonParseError::NoError||!doc.isObject()){QMessageBox::warning(&d,"Invalid profile",error.errorString());return;}
        auto p=publicServerProfile(doc.object());p["id"]=original.value("id");
        if(p.value("baseUrl")==original.value("baseUrl")&&p.value("provider")==original.value("provider")&&p.value("adapter")==original.value("adapter"))p["apiKey"]=original.value("apiKey");
        else {p.remove("apiKey");stageCredentialRemoval(original.value("id").toString());}
        m_profiles[m_current]=p;d.accept();
    });if(d.exec()==QDialog::Accepted)refreshProfiles(m_current);
}
void SettingsDialog::importProfile()
{
    QString path=QFileDialog::getOpenFileName(this,"Import speech-server profile",{},"JSON profile (*.json)");if(path.isEmpty())return;QFile f(path);if(!f.open(QIODevice::ReadOnly)||f.size()>1024*1024){QMessageBox::warning(this,"Import failed","Could not read this profile (maximum 1 MiB).");return;}QJsonParseError e;auto doc=QJsonDocument::fromJson(f.readAll(),&e);if(!doc.isObject()){QMessageBox::warning(this,"Import failed","The file must contain a JSON object.");return;}storeProfile();auto p=publicServerProfile(doc.object());p["id"]=QUuid::createUuid().toString(QUuid::WithoutBraces);if(p.value("name").toString().isEmpty())p["name"]="Imported speech server";m_profiles.append(p);refreshProfiles(m_profiles.size()-1);m_connectionInfo->setText("Profile imported. Review its address and settings before connecting.");
}
void SettingsDialog::exportProfile()
{
    storeProfile();QString path=QFileDialog::getSaveFileName(this,"Export speech-server profile", "speech-server.json","JSON profile (*.json)");if(path.isEmpty())return;QSaveFile f(path);auto bytes=QJsonDocument(publicServerProfile(m_profiles[m_current].toObject())).toJson(QJsonDocument::Indented);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size()||!f.commit())QMessageBox::warning(this,"Export failed",f.errorString());
}
void SettingsDialog::refreshModelPacks()
{
    QStringList names;for(const auto &model:ModelPacks::installedModels())names<<model.name;
    m_modelPackInfo->setText(names.isEmpty()?"No optional models installed. The four included models are ready to use.":"Installed: "+names.join(", "));
    m_modelPackInfo->setToolTip(ModelPacks::modelsDirectory());
}
void SettingsDialog::importModelPack()
{
    const QString path=QFileDialog::getOpenFileName(this,"Import dictation model pack",{},"SpeechDesk model packs (*.zip)");if(path.isEmpty())return;
    ModelPacks importer;
    ModelImportDialog progress(this);progress.setObjectName("modelImportProgress");
    progress.setWindowTitle("Import dictation model");progress.setWindowModality(Qt::WindowModal);progress.setMinimumWidth(430);
    auto layout=new QVBoxLayout(&progress);layout->setContentsMargins(22,22,22,18);layout->setSpacing(16);
    auto label=new QLabel("Checking and importing model pack…");label->setWordWrap(true);layout->addWidget(label);
    auto bar=new QProgressBar;bar->setRange(0,1000);bar->setValue(0);layout->addWidget(bar);
    auto cancel=new QPushButton("Cancel");layout->addWidget(cancel,0,Qt::AlignRight);
    QString failure,installedId;
    progress.requestCancellation=[&]{cancel->setEnabled(false);label->setText("Finishing the current operation…");importer.cancel();};
    connect(cancel,&QPushButton::clicked,&progress,[&]{progress.requestCancellation();});
    connect(&importer,&ModelPacks::progress,&progress,[&](qint64 done,qint64 total){bar->setValue(total>0?int(qMin<qint64>(1000,done*1000/total)):0);});
    connect(&importer,&ModelPacks::completed,&progress,[&](const QString &id){installedId=id;progress.done(QDialog::Accepted);});
    connect(&importer,&ModelPacks::error,&progress,[&](const QString &message){failure=message;progress.done(QDialog::Rejected);});
    connect(&importer,&ModelPacks::cancelled,&progress,[&]{progress.done(QDialog::Rejected);});
    QTimer::singleShot(0,&importer,[&]{importer.importArchive(path);});progress.exec();
    refreshModelPacks();
    if(!failure.isEmpty())QMessageBox::warning(this,"Model import failed",failure);
    else if(!installedId.isEmpty())m_modelPackInfo->setText(m_modelPackInfo->text()+"\nChoose the imported model in Dictate to use it.");
}

#include "mainwindow.h"
#include "settingsdialog.h"
#include "textsegments.h"
#include "modelpacks.h"
#include "credentialstore.h"
#include "credentialdialogs.h"
#include "providerlifecycle.h"
#include <QApplication>
#include <QBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QFileDialog>
#include <QSaveFile>
#include <QJsonDocument>
#include <QMessageBox>
#include <QToolButton>
#include <QStyle>
#include <QStyleHints>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QClipboard>
#include <QShortcut>
#include <QTimer>
#include <QCloseEvent>
#include <QSignalBlocker>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QPainter>
#include <QStylePainter>
#include <QStyleOptionComboBox>
#include <QTextLayout>
#include <QListView>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QScrollArea>
#include <QtMath>
#include <QScreen>
#include <QCryptographicHash>

#ifndef Q_OS_WIN
#include <QDBusMessage>
#include <QDBusConnection>
#include <QDBusVariant>
#endif

static QString durationText(qint64 ms){qint64 s=qMax<qint64>(0,ms/1000);return QString("%1:%2").arg(s/60).arg(s%60,2,10,QChar('0'));}
static QByteArray speechSignature(QJsonObject p){p.remove("discovery");p.remove("discoveryBaseUrl");p.remove("discoverySpeechPath");return QCryptographicHash::hash(QJsonDocument(p).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256);}
static QToolButton *tool(const QString &text,const QString &tip,QWidget *parent=nullptr){auto b=new QToolButton(parent);b->setMinimumHeight(32);b->setText(text);b->setToolTip(tip);b->setAccessibleName(tip);b->setCursor(Qt::PointingHandCursor);return b;}
static QFrame *card(){auto f=new QFrame;f->setObjectName("card");return f;}
static QWidget *field(const QString &label,QWidget *input){auto w=new QWidget;auto l=new QVBoxLayout(w);l->setContentsMargins(0,0,0,0);l->setSpacing(5);auto t=new QLabel(label);t->setProperty("muted",true);t->setBuddy(input);l->addWidget(t);l->addWidget(input);return w;}

namespace {
// Measure the polished control and every complete label. Long selections wrap
// inside the available row; popup rows also wrap instead of inserting ellipses.
class FullTextComboBox final : public QComboBox {
public:
    FullTextComboBox()
    {
        setSizeAdjustPolicy(QComboBox::AdjustToContents);
        setMinimumHeight(36);
        QSizePolicy policy(QSizePolicy::Preferred,QSizePolicy::Preferred);
        policy.setHeightForWidth(true);setSizePolicy(policy);
        auto list=new QListView;list->setTextElideMode(Qt::ElideNone);list->setWordWrap(true);
        list->setUniformItemSizes(false);list->setResizeMode(QListView::Adjust);setView(list);
        connect(this,&QComboBox::currentTextChanged,this,[this]{updateGeometry();updateEditor();update();});
    }
    QSize sizeHint() const override
    {
        ensurePolished();
        QStyleOptionComboBox option;initStyleOption(&option);
        const QFontMetrics metrics(font(),this);int textWidth=metrics.horizontalAdvance(displayText());
        for(int i=0;i<count();++i)textWidth=qMax(textWidth,metrics.horizontalAdvance(itemText(i)));
        return style()->sizeFromContents(QStyle::CT_ComboBox,&option,QSize(textWidth+4,metrics.height()),this).expandedTo(QSize(0,36));
    }
    QSize minimumSizeHint() const override
    {
        ensurePolished();QStyleOptionComboBox option;initStyleOption(&option);
        return style()->sizeFromContents(QStyle::CT_ComboBox,&option,QSize(fontMetrics().maxWidth()+4,fontMetrics().height()),this).expandedTo(QSize(0,36));
    }
    int heightForWidth(int width) const override
    {
        ensurePolished();const QSize natural=sizeHint();
        QStyleOptionComboBox option;initStyleOption(&option);option.rect=QRect(0,0,width,natural.height());
        const QRect field=style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,this);
        QTextLayout text(displayText(),font(),this);
        return qMax(natural.height(),qCeil(wrapText(text,field.width()-4))+natural.height()-field.height());
    }
    void showPopup() override
    {
        const int available=screen()?screen()->availableGeometry().width()-24:width();
        view()->setMinimumWidth(qMin(qMax(width(),sizeHint().width()),available));
        QComboBox::showPopup();
    }
protected:
    void resizeEvent(QResizeEvent *event) override {QComboBox::resizeEvent(event);updateEditor();}
    void focusInEvent(QFocusEvent *event) override
    {
        if(lineEdit())lineEdit()->show();QComboBox::focusInEvent(event);updateEditor();
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if(lineEdit()&&event->position().x()<width()-24){lineEdit()->show();lineEdit()->setFocus();}
        QComboBox::mousePressEvent(event);
    }
    bool eventFilter(QObject *object,QEvent *event) override
    {
        if(object==lineEdit()&&(event->type()==QEvent::FocusOut||event->type()==QEvent::FocusIn))
            QTimer::singleShot(0,this,[this]{updateEditor();update();});
        return QComboBox::eventFilter(object,event);
    }
    void paintEvent(QPaintEvent *event) override
    {
        updateEditor();
        QStyleOptionComboBox option;initStyleOption(&option);
        const QRect field=style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,this).adjusted(2,0,-2,0);
        if((lineEdit()&&lineEdit()->isVisible())||(!isEditable()&&fontMetrics().horizontalAdvance(displayText())<=field.width()&&!displayText().contains('\n'))) {
            QComboBox::paintEvent(event);return;
        }
        QStylePainter painter(this);painter.drawComplexControl(QStyle::CC_ComboBox,option);
        painter.setPen(option.palette.color(isEnabled()?QPalette::Active:QPalette::Disabled,currentText().isEmpty()?QPalette::PlaceholderText:QPalette::ButtonText));
        QTextLayout text(displayText(),font(),this);const qreal height=wrapText(text,field.width());
        painter.setClipRect(field);text.draw(&painter,QPointF(field.x(),field.y()+(field.height()-height)/2.0));
    }
private:
    QString displayText() const {return currentText().isEmpty()&&lineEdit()?lineEdit()->placeholderText():currentText();}
    void updateEditor()
    {
        if(!lineEdit())return;
        lineEdit()->installEventFilter(this);
        QStyleOptionComboBox option;initStyleOption(&option);
        const QRect field=style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,this);
        const bool wraps=fontMetrics().horizontalAdvance(displayText())>field.width()-4||displayText().contains('\n');
        lineEdit()->setVisible(!wraps||lineEdit()->hasFocus());
    }
    static qreal wrapText(QTextLayout &text,int width)
    {
        QTextOption option;option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);text.setTextOption(option);
        qreal height=0;text.beginLayout();
        for(QTextLine line=text.createLine();line.isValid();line=text.createLine()) {
            line.setLineWidth(qMax(1,width));line.setPosition(QPointF(0,height));height+=line.height();
        }
        text.endLayout();return height;
    }
};

// Keep the action row visible when a device name needs more vertical space
// than the compact window can provide. Only the selector row scrolls.
class SelectorScrollArea final : public QScrollArea {
public:
    SelectorScrollArea()
    {
        setFrameShape(QFrame::NoFrame);setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setMinimumHeight(36);viewport()->setAutoFillBackground(false);
        QSizePolicy policy(QSizePolicy::Expanding,QSizePolicy::Preferred);
        policy.setHeightForWidth(true);setSizePolicy(policy);
    }
    void setSelectors(QWidget *selectors)
    {
        setWidget(selectors);selectors->setAutoFillBackground(false);selectors->installEventFilter(this);
    }
    QSize minimumSizeHint() const override {return {0,36};}
    QSize sizeHint() const override {return widget()?widget()->sizeHint().expandedTo(QSize(0,36)):QSize(0,36);}
    int heightForWidth(int width) const override {return widget()?qMax(36,widget()->heightForWidth(qMax(1,width))):36;}
protected:
    void resizeEvent(QResizeEvent *event) override {QScrollArea::resizeEvent(event);keepFullContentHeight();}
    bool eventFilter(QObject *object,QEvent *event) override
    {
        const bool handled=QScrollArea::eventFilter(object,event);
        if(object==widget()&&event->type()==QEvent::LayoutRequest){keepFullContentHeight();updateGeometry();}
        return handled;
    }
private:
    void keepFullContentHeight()
    {
        if(widget())widget()->setMinimumHeight(qMax(36,widget()->heightForWidth(qMax(1,viewport()->width()))));
    }
};

// Rows use real widget hints and remain independently centered.
// The minimum width is the widest individual control's wrapped minimum, not
// the sum of a fixed row, so the window keeps its normal compact size.
class CenteredDictationLayout final : public QLayout {
public:
    explicit CenteredDictationLayout(QWidget *parent,bool expandRows=false):QLayout(parent),m_expandRows(expandRows){setSpacing(7);}
    ~CenteredDictationLayout() override {while(auto item=takeAt(0))delete item;}
    void addItem(QLayoutItem *item) override {m_items.append(item);invalidate();}
    int count() const override {return m_items.size();}
    QLayoutItem *itemAt(int index) const override {return m_items.value(index);}
    QLayoutItem *takeAt(int index) override {return index>=0&&index<m_items.size()?m_items.takeAt(index):nullptr;}
    Qt::Orientations expandingDirections() const override {return Qt::Horizontal;}
    bool hasHeightForWidth() const override {return true;}
    int heightForWidth(int width) const override {return arrange(QRect(0,0,width,0),false);}
    QSize minimumSize() const override
    {
        QSize size(0,0);for(auto item:m_items)if(!item->isEmpty())size=size.expandedTo(item->minimumSize());
        const auto margins=contentsMargins();return size+QSize(margins.left()+margins.right(),margins.top()+margins.bottom());
    }
    QSize sizeHint() const override
    {
        QSize size(0,0);int visible=0;
        for(auto item:m_items)if(!item->isEmpty()) {
            const QSize hint=item->sizeHint().expandedTo(item->minimumSize());
            size.rwidth()+=hint.width()+(visible++?spacing():0);size.setHeight(qMax(size.height(),hint.height()));
        }
        const auto margins=contentsMargins();return size+QSize(margins.left()+margins.right(),margins.top()+margins.bottom());
    }
    void setGeometry(const QRect &rect) override {QLayout::setGeometry(rect);arrange(rect,true);}
private:
    QList<QLayoutItem *> m_items;
    bool m_expandRows;
    int arrange(const QRect &rect,bool apply) const
    {
        const auto margins=contentsMargins();const int available=qMax(1,rect.width()-margins.left()-margins.right());
        struct Entry {QLayoutItem *item;QSize size;};
        QList<Entry> row;int rowWidth=0,rowHeight=0,y=rect.y()+margins.top();
        auto finishRow=[&]{
            if(row.isEmpty())return;
            if(m_expandRows) {
                int expanding=0;for(const auto &entry:row)if(entry.item->expandingDirections()&Qt::Horizontal)++expanding;
                int extra=available-rowWidth;
                for(auto &entry:row)if(expanding&&(entry.item->expandingDirections()&Qt::Horizontal)) {
                    const int added=extra/expanding;entry.size.rwidth()+=added;rowWidth+=added;extra-=added;--expanding;
                    if(entry.item->hasHeightForWidth())entry.size.setHeight(qMax(entry.size.height(),entry.item->heightForWidth(entry.size.width())));
                }
            }
            // Action rows use a shared natural height, including larger fonts.
            for(auto &entry:row)if(qobject_cast<QPushButton *>(entry.item->widget()))entry.size.setHeight(rowHeight);
            int x=rect.x()+margins.left()+(available-rowWidth)/2;
            for(const auto &entry:row) {
                const int top=(entry.item->alignment()&Qt::AlignBottom)?rowHeight-entry.size.height():(rowHeight-entry.size.height())/2;
                if(apply)entry.item->setGeometry(QRect(QPoint(x,y+top),entry.size));
                x+=entry.size.width()+spacing();
            }
            y+=rowHeight+spacing();row.clear();rowWidth=rowHeight=0;
        };
        for(auto item:m_items) {
            if(item->isEmpty())continue;
            if(auto widget=item->widget())widget->ensurePolished();
            const QSize hint=item->sizeHint().expandedTo(item->minimumSize());
            const int width=qMin(available,hint.width());
            const int height=qMax(hint.height(),item->hasHeightForWidth()?item->heightForWidth(width):0);
            if(!row.isEmpty()&&rowWidth+spacing()+width>available)finishRow();
            rowWidth+=width+(row.isEmpty()?0:spacing());rowHeight=qMax(rowHeight,height);row.append({item,{width,height}});
        }
        finishRow();return y-rect.y()+margins.bottom()-(y>rect.y()+margins.top()?spacing():0);
    }
};
}
QString MainWindow::configDirectory()
{
    QString override=qEnvironmentVariable("SPEECHDESK_CONFIG_DIR");if(!override.isEmpty())return override;
#ifdef Q_OS_WIN
    return QDir(qEnvironmentVariable("APPDATA",QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))).filePath("Speech-Desk");
#else
    return QDir(qEnvironmentVariable("XDG_CONFIG_HOME",QDir::home().filePath(".config"))).filePath("Speech-Desk");
#endif
}
void MainWindow::loadSettings()
{
    QFile f(QDir(configDirectory()).filePath("settings.json"));if(f.open(QIODevice::ReadOnly)&&f.size()<4*1024*1024)m_settings=QJsonDocument::fromJson(f.readAll()).object();
    if(m_settings.value("profiles").toArray().isEmpty())m_settings["profiles"]=QJsonArray{defaultServerProfile()};
}
void MainWindow::saveSettings()
{
    if(m_demo)return;
    rememberActiveProfile();m_settings["volume"]=m_volume->value();m_settings["live"]=m_live->isChecked();m_settings["sttModel"]=m_sttModel->currentData().toString();m_settings["sttLanguage"]=m_language->currentData().toString();m_settings["device"]=m_device->currentData().toString();m_settings["width"]=width();m_settings["height"]=height();
    QString error;if(!writeSettings(m_settings,&error))m_status->setText("Could not save settings: "+error);
}
bool MainWindow::writeSettings(const QJsonObject &settings,QString *error)
{
    if(m_demo)return true;
    QJsonObject disk=settings;QJsonArray profiles;
    for(auto p:disk["profiles"].toArray())profiles.append(publicServerProfile(p.toObject()));
    disk["profiles"]=profiles;
    disk["volume"]=m_volume->value();disk["live"]=m_live->isChecked();disk["sttModel"]=m_sttModel->currentData().toString();
    disk["sttLanguage"]=m_language->currentData().toString();disk["device"]=m_device->currentData().toString();disk["width"]=width();disk["height"]=height();
    if(!QDir().mkpath(configDirectory())){if(error)*error="Could not create the settings directory.";return false;}
    QSaveFile f(QDir(configDirectory()).filePath("settings.json"));auto bytes=QJsonDocument(disk).toJson(QJsonDocument::Indented);
    if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size()||!f.commit()){if(error)*error=f.errorString();return false;}
#ifndef Q_OS_WIN
    QFile::setPermissions(QDir(configDirectory()).filePath("settings.json"),QFileDevice::ReadOwner|QFileDevice::WriteOwner);
#endif
    return true;
}

MainWindow::MainWindow(QWidget *parent):QMainWindow(parent),m_tts(this),m_stt(this),m_merger(this),m_player(this),m_output(this)
{
    m_lifecycleDate=ProviderLifecycle::utcToday();loadSettings();setWindowTitle("SpeechDesk");setWindowIcon(QIcon(":/app.svg"));setMinimumSize(680,540);
    QSize screen=QGuiApplication::primaryScreen()?QGuiApplication::primaryScreen()->availableGeometry().size():QSize(1400,900);
    resize(qMin(m_settings.value("width").toInt(1100),screen.width()),qMin(m_settings.value("height").toInt(760),screen.height()));
    auto central=new QWidget;auto outer=new QVBoxLayout(central);outer->setContentsMargins(18,12,18,12);outer->setSpacing(10);setCentralWidget(central);
    auto header=new QHBoxLayout;auto mark=new QLabel;mark->setPixmap(QIcon(":/app.svg").pixmap(42,42));header->addWidget(mark);auto labels=new QVBoxLayout;labels->setSpacing(1);auto name=new QLabel("SpeechDesk");name->setObjectName("brand");labels->addWidget(name);auto subtitle=new QLabel("Write, dictate, then listen");subtitle->setProperty("muted",true);labels->addWidget(subtitle);header->addLayout(labels);header->addStretch();
    auto settings=tool("", "Settings");settings->setObjectName("settingsButton");settings->setIcon(QIcon(":/settings.svg"));settings->setIconSize(QSize(22,22));settings->setFixedSize(40,40);header->addWidget(settings);connect(settings,&QToolButton::clicked,this,[this]{configure();});outer->addLayout(header);
    outer->addWidget(buildTts(),1);
    m_player.setAudioOutput(&m_output);m_output.setVolume(m_volume->value()/100.0);
    connect(m_volume,&QSlider::valueChanged,this,[this](int v){m_output.setVolume(v/100.0);});
    connect(m_seek,&QSlider::sliderMoved,this,[this](int value){if(m_player.isSeekable())m_player.setPosition(value);});
    connect(&m_player,&QMediaPlayer::positionChanged,this,[this](qint64 pos){if(!m_seek->isSliderDown())m_seek->setValue(int(pos));m_time->setText(durationText(pos)+" / "+durationText(m_player.duration()));updateHighlight(pos);});
    connect(&m_player,&QMediaPlayer::durationChanged,this,[this](qint64 d){m_seek->setRange(0,int(qMin<qint64>(d,INT_MAX)));m_time->setText(durationText(m_player.position())+" / "+durationText(d));});
    connect(&m_player,&QMediaPlayer::seekableChanged,m_seek,&QSlider::setEnabled);
    connect(&m_player,&QMediaPlayer::mediaStatusChanged,this,[this](QMediaPlayer::MediaStatus s){if(s==QMediaPlayer::EndOfMedia&&m_active){auto generation=m_playbackGeneration;int current=m_current;auto source=m_player.source();QTimer::singleShot(0,this,[this,generation,current,source]{if(m_active&&generation==m_playbackGeneration&&current==m_current&&source==m_player.source())nextSection();});}});
    connect(&m_player,&QMediaPlayer::errorOccurred,this,[this](QMediaPlayer::Error e,const QString &s){if(e!=QMediaPlayer::NoError&&m_active)speechError("Audio playback failed: "+s);});
    connect(&m_tts,&TtsClient::status,this,[this](const QString &s){if(m_active&&m_current>=0&&m_audioPaths.value(m_current).isEmpty())m_status->setText(s);});
    connect(&m_tts,&TtsClient::error,this,&MainWindow::speechError);
    connect(&m_tts,&TtsClient::probeFinished,this,[this](const QJsonObject &info){
        if(!m_active||!m_detecting)return;m_detecting=false;
        auto ps=m_settings["profiles"].toArray();int i=m_server->currentIndex();if(i>=0&&i<ps.size()){auto p=selectedProfile();p["discovery"]=info;p["discoveryBaseUrl"]=p["baseUrl"];p["discoverySpeechPath"]=p.value("speechPath").toString("/v1/audio/speech");ps[i]=p;m_settings["profiles"]=ps;updateVoiceChoices();}
        requestNext();
    });
    connect(&m_tts,&TtsClient::audioReady,this,[this](const QByteArray &audio,const QString &format,const QVector<WordCue> &cues){
        if(!m_active||m_request<0||!m_audioDir)return;int i=m_request;m_requestBusy=false;m_request=-1;
        QString suffix=QStringList{"wav","mp3","flac","ogg","opus","aac","m4a"}.contains(format)?format:"wav";
        QString path=QDir(m_audioDir->path()).filePath(QString("section-%1.%2").arg(i+1,5,10,QChar('0')).arg(suffix));QFile f(path);
        if(!f.open(QIODevice::WriteOnly)||f.write(audio)!=audio.size()){speechError("Could not store temporary audio: "+f.errorString());return;}f.close();
        m_audioPaths[i]=path;m_audioFormats[i]=suffix;m_cues[i]=cues;
        if(i==m_current)playCurrent();requestNext();
    });
    connect(&m_merger,&AudioMerger::progress,this,[this](int n,int total){m_status->setText(QString("Saving audio · part %1 of %2").arg(n).arg(total));m_export->setEnabled(false);updateSttControls();});
    connect(&m_merger,&AudioMerger::completed,this,[this](const QString &path){m_status->setText("Audio saved: "+QFileInfo(path).fileName());m_export->setEnabled(m_cacheComplete);m_speak->setEnabled(!m_active&&!m_sttBusy&&!m_recording&&!m_text->toPlainText().trimmed().isEmpty());updateSttControls();});
    connect(&m_merger,&AudioMerger::error,this,[this](const QString &s){m_status->setText("Export failed: "+s);m_export->setEnabled(m_cacheComplete);m_speak->setEnabled(!m_active&&!m_sttBusy&&!m_recording&&!m_text->toPlainText().trimmed().isEmpty());updateSttControls();});
    connect(&m_stt,&SttService::status,this,[this](const QString &message){
        if(m_dictationActive&&!m_dictationFinal)m_sttStatus->setText(message);
    });
    connect(&m_stt,&SttService::error,this,[this](const QString &message){
        if(m_dictationActive)finishDictation(false);
        m_sttStatus->setText(message);m_sttStatus->setToolTip(message);
    });
    connect(&m_stt,&SttService::devicesError,this,[this](const QString &message){
        if(!m_dictationActive&&!m_active){m_sttStatus->setText(message);m_sttStatus->setToolTip(message);}
    });
    connect(&m_stt,&SttService::finished,this,[this]{
        if(m_dictationActive)finishDictation(m_dictationFinal);
        m_recording=false;m_sttBusy=false;updateSttControls();
    });
    connect(&m_stt,&SttService::recordingChanged,this,[this](bool active){m_recording=active;updateSttControls();});
    connect(&m_stt,&SttService::transcript,this,&MainWindow::acceptDictation);
    connect(&m_stt,&SttService::levelChanged,this,[this](double peak,double seconds){m_level->setValue(qBound(0,int(peak*100),100));m_recordTime->setText(durationText(qint64(seconds*1000)));});
    connect(&m_stt,&SttService::devicesReady,this,[this](const QJsonArray &devices){QSignalBlocker b(m_device);QString saved=m_device->currentData().toString();if(saved.isEmpty())saved=m_settings.value("device").toString();m_device->clear();m_device->addItem("System default microphone",QString());for(auto d:devices){auto o=d.toObject();m_device->addItem(o["name"].toString(),o["id"].toString());}m_device->setCurrentIndex(qMax(0,m_device->findData(saved)));});
    QString resources=qEnvironmentVariable("SPEECHDESK_RESOURCES");if(resources.isEmpty()){resources=QDir(QCoreApplication::applicationDirPath()).filePath("resources");if(!QDir(resources).exists())resources="/usr/lib/speechdesk/resources";}m_stt.setResourceRoot(resources);
    updateProfileChoices();applyTheme();updateStatistics();updateSttControls();
    connect(m_model,&QComboBox::editTextChanged,this,[this]{updateModelVoices(true);rememberActiveProfile();});connect(m_voice,&QComboBox::editTextChanged,this,[this]{rememberActiveProfile();});
    auto themeTimer=new QTimer(this);connect(themeTimer,&QTimer::timeout,this,[this]{
        if(m_settings.value("theme").toString("system")=="system")applyTheme();
        const auto day=ProviderLifecycle::utcToday();
        if(day!=m_lifecycleDate) {
            m_lifecycleDate=day;const auto profile=selectedProfile();
            if(m_active&&!ProviderLifecycle::modelAllowed(profile,profile.value("model").toString()))stopSpeech();
            updateProfileChoices();
        }
    });themeTimer->start(5000);
    auto open=new QShortcut(QKeySequence::Open,this);connect(open,&QShortcut::activated,this,&MainWindow::openText);
    auto save=new QShortcut(QKeySequence::Save,this);connect(save,&QShortcut::activated,this,&MainWindow::saveText);
    auto saveAudioShortcut=new QShortcut(QKeySequence("Ctrl+Shift+S"),this);connect(saveAudioShortcut,&QShortcut::activated,this,&MainWindow::saveAudio);
    auto speak=new QShortcut(QKeySequence("Ctrl+Return"),this);connect(speak,&QShortcut::activated,this,[this]{if(!m_active)startSpeech();else pauseSpeech();});
    auto rec=new QShortcut(QKeySequence("Ctrl+R"),this);connect(rec,&QShortcut::activated,this,&MainWindow::record);
    auto cancel=new QShortcut(QKeySequence(Qt::Key_Escape),this);connect(cancel,&QShortcut::activated,this,[this]{
        if(m_active)stopSpeech();
        if(m_dictationActive||m_sttBusy||m_recording)cancelDictation();
        if(m_merger.busy()){m_merger.cancel();m_export->setEnabled(m_cacheComplete);m_speak->setEnabled(!m_active&&!m_sttBusy&&!m_text->toPlainText().trimmed().isEmpty());m_status->setText("Audio export cancelled.");updateSttControls();}
    });
}
QWidget *MainWindow::buildTts()
{
    auto page=new QWidget;auto l=new QVBoxLayout(page);l->setContentsMargins(0,0,0,0);l->setSpacing(8);
    auto choicePanel=new QWidget;choicePanel->setObjectName("speechChoices");auto choices=new CenteredDictationLayout(choicePanel,true);choices->setContentsMargins(0,0,0,0);choices->setSpacing(12);
    m_server=new FullTextComboBox;m_server->setObjectName("speechServer");m_model=new FullTextComboBox;m_model->setObjectName("speechModel");m_model->setEditable(true);m_model->setInsertPolicy(QComboBox::NoInsert);m_model->lineEdit()->setPlaceholderText("Server default model");m_voice=new FullTextComboBox;m_voice->setObjectName("speechVoice");m_voice->setEditable(true);m_voice->setInsertPolicy(QComboBox::NoInsert);m_voice->lineEdit()->setPlaceholderText("Server default voice");
    for(auto box:{m_server,m_model,m_voice}){auto policy=box->sizePolicy();policy.setHorizontalPolicy(QSizePolicy::Expanding);box->setSizePolicy(policy);}
    choices->addWidget(field("Server",m_server));choices->addWidget(field("Model",m_model));choices->addWidget(field("Voice",m_voice));auto setup=tool("Configure","Configure speech servers");choices->addWidget(setup);choices->itemAt(choices->count()-1)->setAlignment(Qt::AlignBottom);connect(setup,&QToolButton::clicked,this,[this]{configure(true);});l->addWidget(choicePanel);
    connect(m_server,&QComboBox::currentIndexChanged,this,[this](int i){m_settings["selectedProfile"]=i;updateVoiceChoices();});
    auto editorCard=card();auto el=new QVBoxLayout(editorCard);el->setContentsMargins(0,0,0,0);el->setSpacing(0);
    auto toolbar=new QHBoxLayout;toolbar->setContentsMargins(12,4,10,4);
    auto label=new QLabel("TEXT TO READ");label->setProperty("sectionLabel",true);toolbar->addWidget(label);toolbar->addStretch();
    m_dictate=tool("Dictate","Show dictation controls · insert speech at the cursor (Ctrl+R)");m_dictate->setObjectName("dictateButton");m_dictate->setCheckable(true);
    m_openText=tool("Open TXT","Open a text file (Ctrl+O)");m_openText->setObjectName("openTextButton");
    m_saveText=tool("Save text","Save the editor as a text file (Ctrl+S)");m_saveText->setObjectName("saveTextButton");
    m_clearText=tool("Clear","Clear the text");m_clearText->setObjectName("clearTextButton");
    toolbar->addWidget(m_dictate);toolbar->addWidget(m_openText);toolbar->addWidget(m_saveText);toolbar->addWidget(m_clearText);el->addLayout(toolbar);
    m_dictationPanel=buildDictation();m_dictationPanel->setObjectName("dictationPanel");m_dictationPanel->setVisible(false);el->addWidget(m_dictationPanel);
    m_text=new NumberedEditor;m_text->setObjectName("speechEditor");m_text->setMinimumHeight(54);
    m_text->setPlaceholderText("Type, paste, open a TXT file, or choose Dictate.\n\nYour words stay here. Edit them, then press Speak to listen.");m_text->setAccessibleName("Text to read or dictate");el->addWidget(m_text,1);
    auto bottom=new QHBoxLayout;bottom->setContentsMargins(12,4,12,4);m_stats=new QLabel;m_stats->setProperty("muted",true);bottom->addWidget(m_stats);bottom->addStretch();m_highlightInfo=new QLabel("Playback highlighting on");m_highlightInfo->setProperty("muted",true);bottom->addWidget(m_highlightInfo);el->addLayout(bottom);l->addWidget(editorCard,1);
    connect(m_dictate,&QToolButton::toggled,this,[this](bool visible){
        m_dictationPanel->setVisible(visible);updateMeterVisibility();
        if(visible&&!m_sttBusy&&!m_active){m_status->setText("Dictate at the cursor, or replace selected text. Recognition stays on this computer.");if(m_device->count()==1)m_stt.requestDevices();}
    });
    connect(m_openText,&QToolButton::clicked,this,&MainWindow::openText);connect(m_saveText,&QToolButton::clicked,this,&MainWindow::saveText);
    connect(m_clearText,&QToolButton::clicked,this,[this]{if(m_active||m_dictationActive||m_sttBusy)return;if(confirmReplace(m_text,"Clear text"))m_text->clear();});
    connect(m_text,&QPlainTextEdit::textChanged,this,&MainWindow::updateStatistics);
    connect(m_text,&QPlainTextEdit::selectionChanged,this,[this]{if(!m_active&&!m_dictationActive)m_speak->setText(m_text->textCursor().hasSelection()?"Speak selection":"Speak");});
    auto controls=new QHBoxLayout;controls->setSpacing(9);m_speak=new QPushButton("Speak");m_speak->setObjectName("speakButton");m_speak->setProperty("accent",true);m_speak->setMinimumWidth(118);m_speak->setMinimumHeight(36);m_speak->setToolTip("Read text or selected text (Ctrl+Enter)");m_pause=new QPushButton("Pause");m_pause->setMinimumHeight(36);m_stop=new QPushButton("Stop");m_stop->setMinimumHeight(36);controls->addWidget(m_speak);controls->addWidget(m_pause);controls->addWidget(m_stop);controls->addStretch();m_export=new QPushButton("Save audio");m_export->setMinimumHeight(36);m_export->setEnabled(false);controls->addWidget(m_export);auto volume=new QLabel("Volume");volume->setProperty("muted",true);controls->addWidget(volume);m_volume=new QSlider(Qt::Horizontal);m_volume->setRange(0,100);m_volume->setValue(m_settings.value("volume").toInt(85));m_volume->setFixedWidth(80);m_volume->setAccessibleName("Playback volume");controls->addWidget(m_volume);l->addLayout(controls);
    auto progress=new QHBoxLayout;m_seek=new QSlider(Qt::Horizontal);m_seek->setRange(0,0);m_seek->setEnabled(false);m_seek->setAccessibleName("Position within current reading part");m_time=new QLabel("0:00 / 0:00");m_time->setProperty("muted",true);m_time->setMinimumWidth(88);progress->addWidget(m_seek,1);progress->addWidget(m_time);l->addLayout(progress);auto statusRow=new QWidget;statusRow->setObjectName("speechStatusBar");auto statusLayout=new QGridLayout(statusRow);statusLayout->setContentsMargins(0,0,0,0);statusLayout->setHorizontalSpacing(8);
    m_status=new QLabel("Add a speech server in Settings to begin.");m_status->setWordWrap(true);m_status->setProperty("muted",true);m_status->setMinimumHeight(28);m_status->setMinimumWidth(0);m_status->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);m_sttStatus=m_status;
    m_level=new SymmetricLevelMeter;m_level->setObjectName("microphoneLevel");m_level->setFixedSize(200,16);m_level->setVisible(false);
    m_recordTime=new QLabel("0:00");m_recordTime->setObjectName("recordingTime");m_recordTime->setProperty("muted",true);m_recordTime->setAlignment(Qt::AlignRight|Qt::AlignVCenter);m_recordTime->setMinimumWidth(0);m_recordTime->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);m_recordTime->setVisible(false);
    statusLayout->addWidget(m_status,0,0,1,3);statusLayout->addWidget(m_level,0,1,Qt::AlignCenter);statusLayout->addWidget(m_recordTime,0,2);statusLayout->setColumnStretch(0,1);statusLayout->setColumnStretch(2,1);l->addWidget(statusRow);
    connect(m_speak,&QPushButton::clicked,this,&MainWindow::startSpeech);connect(m_pause,&QPushButton::clicked,this,&MainWindow::pauseSpeech);connect(m_stop,&QPushButton::clicked,this,&MainWindow::stopSpeech);connect(m_export,&QPushButton::clicked,this,&MainWindow::saveAudio);m_export->setToolTip("Save the complete generated reading as WAV (Ctrl+Shift+S)");m_pause->setEnabled(false);m_stop->setEnabled(false);
    return page;
}
QWidget *MainWindow::buildDictation()
{
    auto panel=new QWidget;auto insetLayout=new QVBoxLayout(panel);insetLayout->setContentsMargins(12,2,12,8);
    auto inset=new QFrame;inset->setObjectName("dictationInset");insetLayout->addWidget(inset);
    auto rows=new QVBoxLayout(inset);rows->setContentsMargins(12,10,12,10);rows->setSpacing(9);
    auto selectors=new QWidget;selectors->setObjectName("dictationSelectors");auto layout=new QHBoxLayout(selectors);layout->setContentsMargins(0,0,0,0);layout->setSpacing(7);
    auto selectorScroll=new SelectorScrollArea;selectorScroll->setObjectName("dictationSelectorScroll");selectorScroll->setSelectors(selectors);rows->addWidget(selectorScroll);
    m_sttModel=new FullTextComboBox;m_sttModel->setObjectName("recognitionModel");
    m_sttModel->setAccessibleName("Dictation recognition model");m_sttModel->setToolTip("Local recognition model");
    refreshDictationModels();
    m_language=new FullTextComboBox;m_language->setObjectName("recognitionLanguage");m_language->setAccessibleName("Dictation language");m_language->setToolTip("Language to recognize");
    m_device=new FullTextComboBox;m_device->setObjectName("microphoneCombo");m_device->setAccessibleName("Dictation microphone");m_device->addItem("System default microphone",QString());m_device->setToolTip("Microphone input");
    for(auto box:{m_sttModel,m_language,m_device}){auto policy=box->sizePolicy();policy.setHorizontalPolicy(QSizePolicy::Expanding);box->setSizePolicy(policy);}
    layout->addWidget(m_sttModel,3);layout->addWidget(m_language,2);layout->addWidget(m_device,4);
    m_refreshDevices=tool("","Refresh microphones");m_refreshDevices->setObjectName("refreshMicrophonesButton");m_refreshDevices->setIcon(QIcon(":/refresh.svg"));m_refreshDevices->setIconSize(QSize(18,18));m_refreshDevices->setFixedSize(36,36);layout->addWidget(m_refreshDevices);
    connect(m_refreshDevices,&QToolButton::clicked,this,[this]{if(!m_sttBusy&&!m_recording)m_stt.requestDevices();});
    connect(m_sttModel,&QComboBox::currentIndexChanged,this,&MainWindow::updateLanguages);updateLanguages();
    m_record=new QPushButton("Record");m_record->setObjectName("recordButton");m_record->setToolTip("Dictate into the editor at the cursor (Ctrl+R); Stop finishes recognition");
    m_cancelStt=new QPushButton("Cancel");m_cancelStt->setObjectName("cancelDictationButton");m_cancelStt->setToolTip("Discard this dictation and restore the original text (Escape)");
    m_transcribeFile=new QPushButton("Audio file");m_transcribeFile->setObjectName("insertAudioButton");m_transcribeFile->setToolTip("Insert words from an audio recording at the cursor");
    m_live=new AnimatedToggleButton("Live text");m_live->setObjectName("liveDictation");m_live->setChecked(m_settings.value("live").toBool(true));m_live->setToolTip("Update text while speaking, then refine it after Stop. Turn off to transcribe only after recording.");
    auto actions=new QWidget;actions->setObjectName("dictationActions");auto actionLayout=new QHBoxLayout(actions);actionLayout->setContentsMargins(0,0,0,0);actionLayout->setSpacing(7);actionLayout->addStretch();rows->addWidget(actions);
    for(auto button:QList<QPushButton *>{m_record,m_cancelStt,m_transcribeFile,m_live}){button->setMinimumHeight(36);button->setSizePolicy(QSizePolicy::Minimum,QSizePolicy::Preferred);actionLayout->addWidget(button);}
    actionLayout->addStretch();
    connect(m_record,&QPushButton::clicked,this,&MainWindow::record);connect(m_transcribeFile,&QPushButton::clicked,this,&MainWindow::openAudio);connect(m_cancelStt,&QPushButton::clicked,this,&MainWindow::cancelDictation);
    return panel;
}
void MainWindow::updateProfileChoices()
{
    QSignalBlocker b(m_server);m_server->clear();auto profiles=m_settings["profiles"].toArray();for(int i=0;i<profiles.size();++i){auto o=currentServerProfile(profiles[i].toObject());profiles[i]=o;m_server->addItem(o["name"].toString("Speech server"),o["id"].toString());}m_settings["profiles"]=profiles;m_server->setCurrentIndex(qBound(0,m_settings["selectedProfile"].toInt(),m_server->count()-1));updateVoiceChoices();
}
void MainWindow::updateVoiceChoices()
{
    auto profiles=m_settings["profiles"].toArray();int i=m_server->currentIndex();if(i<0||i>=profiles.size())return;auto p=profiles[i].toObject();QSignalBlocker b1(m_model),b2(m_voice);m_model->clear();m_voice->clear();auto discovery=p["discovery"].toObject();
    for(auto item:ProviderLifecycle::modelChoices(p,discovery["models"].toArray())){auto o=item.toObject();m_model->addItem(o["id"].toString());}
    for(auto item:discovery["voices"].toArray()){auto o=item.toObject();m_voice->addItem(o["id"].toString());}
    const bool allowed=!p.value("modelRetired").toBool()&&ProviderLifecycle::modelAllowed(p,p.value("model").toString());
    m_model->setCurrentText(allowed?p["model"].toString():QString());
    m_model->lineEdit()->setPlaceholderText(allowed?"Server default model":"Retired model — choose replacement");
    m_voice->setCurrentText(p["voice"].toString());updateModelVoices();m_server->setToolTip(p["baseUrl"].toString());
    if(!m_active)m_status->setText(!allowed?"This OpenAI model is retired. Select a supported model to continue.":(p["baseUrl"].toString().isEmpty()?"Add a speech server in Settings to begin.":"Ready · Text will be sent to "+p["name"].toString("the selected server")+" when you press Speak."));
}
void MainWindow::updateModelVoices(bool modelChanged)
{
    const auto profile=selectedProfile();
    if(profile.value("adapter").toString()!="openrouter"&&!ProviderLifecycle::isOfficialOpenAi(profile))return;
    const QString previous=m_voice->currentText();
    QSignalBlocker block(m_voice);m_voice->clear();
    for(const auto &entry:profile.value("discovery").toObject().value("models").toArray()) {
        const auto model=entry.toObject();
        if(model.value("id").toString()!=m_model->currentText().trimmed())continue;
        for(const auto &voice:model.value("voices").toArray())m_voice->addItem(voice.toObject().value("id").toString());
        break;
    }
    if(!modelChanged||m_voice->findText(previous)>=0)m_voice->setCurrentText(previous);
    else if(m_voice->count()>0)m_voice->setCurrentIndex(0);
    else m_voice->setCurrentText(QString());
}
QJsonObject MainWindow::selectedProfile() const
{
    auto profiles=m_settings["profiles"].toArray();int i=m_server->currentIndex();if(i<0||i>=profiles.size())return {};auto p=profiles[i].toObject();p["model"]=m_model->currentText().trimmed();p["voice"]=m_voice->currentText().trimmed();if(!p.value("model").toString().isEmpty()&&ProviderLifecycle::modelAllowed(p,p.value("model").toString()))p.remove("modelRetired");return p;
}
void MainWindow::rememberActiveProfile(){auto ps=m_settings["profiles"].toArray();int i=m_server->currentIndex();if(i>=0&&i<ps.size()){ps[i]=selectedProfile();m_settings["profiles"]=ps;m_settings["selectedProfile"]=i;}}
void MainWindow::updateStatistics()
{
    auto text=m_text->toPlainText();auto words=text.split(QRegularExpression("\\s+"),Qt::SkipEmptyParts).size();m_stats->setText(QString("%1 characters · %2 words").arg(text.size()).arg(words));m_speak->setEnabled(!m_active&&!m_dictationActive&&!m_sttBusy&&!m_recording&&!m_merger.busy()&&!text.trimmed().isEmpty());m_saveText->setEnabled(!m_dictationActive&&!text.isEmpty());
    if(!m_active){m_cacheComplete=false;m_export->setEnabled(false);m_highlightInfo->setText(m_settings.value("highlight").toBool(true)?"Playback highlighting on":"Highlighting off");}
}
void MainWindow::setSpeechActive(bool active)
{
    m_active=active;m_pause->setEnabled(active);m_stop->setEnabled(active);m_server->setEnabled(!active);m_model->setEnabled(!active);m_voice->setEnabled(!active);m_pause->setText(m_paused?"Resume":"Pause");updateSttControls();
}
void MainWindow::lockProfileCredential(const QString &id)
{
    auto profiles=m_settings.value("profiles").toArray();
    for(int i=0;i<profiles.size();++i)if(profiles[i].toObject().value("id").toString()==id)profiles[i]=publicServerProfile(profiles[i].toObject());
    m_settings["profiles"]=profiles;m_tts.setProfile({});
    m_cachedProfile.fill('\0');m_cachedProfile.clear();
}
bool MainWindow::unlockSelectedCredential()
{
    const auto profile=selectedProfile();
    if(!profile.value("apiKey").toString().trimmed().isEmpty())return true;
    const QString directory=QDir(configDirectory()).filePath("credentials");CredentialStore credentials(directory);
    if(!credentials.exists(profile.value("id").toString())) {
        if(profile.value("provider").toString("selfhosted")!="selfhosted"){configure(true);return false;}
        return true;
    }
    QString key;
    if(!unlockCredentialForProfile(this,profile,directory,&key)){m_status->setText("Saved API key remains locked. Unlock it when you are ready to speak.");return false;}
    auto profiles=m_settings["profiles"].toArray();const int index=m_server->currentIndex();
    if(index<0||index>=profiles.size())return false;
    auto unlocked=profiles[index].toObject();unlocked["apiKey"]=key;key.fill(QChar(0));profiles[index]=unlocked;m_settings["profiles"]=profiles;return true;
}
void MainWindow::startSpeech()
{
    if(m_active||m_dictationActive||m_sttBusy||m_recording||m_merger.busy())return;auto profile=selectedProfile();QUrl url(profile["baseUrl"].toString());if(!url.isValid()||url.host().isEmpty()||!(url.scheme()=="http"||url.scheme()=="https")){configure(true);return;}
    auto cursor=m_text->textCursor();QString text=cursor.hasSelection()?cursor.selectedText().replace(QChar::ParagraphSeparator,'\n'):m_text->toPlainText();int offset=cursor.hasSelection()?cursor.selectionStart():0;if(text.trimmed().isEmpty())return;
    if(text.size()>200000){m_status->setText("Select a shorter passage (up to 200,000 characters) for one playback session.");return;}
    if(!ProviderLifecycle::modelAllowed(profile,profile.value("model").toString())){m_status->setText("This OpenAI model is retired. Choose gpt-realtime-2.1-mini or another supported model in Settings.");return;}
    if(ProviderLifecycle::isOfficialOpenAi(profile)&&profile.value("model").toString().trimmed().isEmpty()){m_status->setText("Choose a supported OpenAI model before speaking.");return;}
    if(!unlockSelectedCredential())return;profile=selectedProfile();
    if(m_cacheComplete&&text==m_cachedText&&speechSignature(profile)==m_cachedProfile&&!m_segments.isEmpty()&&m_segments.first().start==offset){++m_playbackGeneration;m_current=0;m_request=-1;m_requestBusy=false;m_paused=false;m_detecting=false;setSpeechActive(true);playCurrent();return;}
    ++m_playbackGeneration;m_tts.cancel();m_player.stop();m_player.setSource(QUrl());m_audioDir=std::make_unique<QTemporaryDir>(QDir::tempPath()+"/speechdesk-playback-XXXXXX");if(!m_audioDir->isValid()){m_status->setText("Could not create a temporary audio folder.");return;}
    m_segments=splitSpeechText(text,profile["maxChars"].toInt(500),offset);if(m_segments.isEmpty())return;
    m_audioPaths=QVector<QString>(m_segments.size());m_audioFormats=QVector<QString>(m_segments.size());m_cues=QVector<QVector<WordCue>>(m_segments.size());m_current=0;m_request=-1;m_requestBusy=false;m_paused=false;m_cacheComplete=false;m_export->setEnabled(false);m_cachedText=text;m_cachedProfile=speechSignature(profile);m_tts.setProfile(profile);m_text->clearReadingRange();setSpeechActive(true);m_status->setText("Generating speech…");m_highlightInfo->setText("Waiting for audio");
    bool discovered=profile["discoveryBaseUrl"]==profile["baseUrl"]&&profile["discoverySpeechPath"].toString()==profile.value("speechPath").toString("/v1/audio/speech");
    m_detecting=profile.value("adapter").toString("auto")=="auto"&&!discovered;
    if(m_detecting){m_status->setText("Checking speech-server capabilities…");m_tts.probe();}else requestNext();
}
void MainWindow::requestNext()
{
    if(!m_active||m_requestBusy||m_detecting)return;
    for(int i=m_current;i<=qMin(m_current+1,int(m_segments.size())-1);++i){if(m_audioPaths[i].isEmpty()){m_request=i;m_requestBusy=true;m_tts.synthesize(m_segments[i].text);return;}}
}
void MainWindow::playCurrent()
{
    if(!m_active||m_current<0||m_current>=m_segments.size())return;
    if(m_audioPaths[m_current].isEmpty()){m_text->clearReadingRange();m_status->setText(QString("Generating speech…"));requestNext();return;}
    m_player.setSource(QUrl::fromLocalFile(m_audioPaths[m_current]));
    m_status->setText(QString(m_paused?"Paused · part %1 of %2":"Reading part %1 of %2").arg(m_current+1).arg(m_segments.size()));m_highlightInfo->setText(m_cues[m_current].isEmpty()?"Sentence highlighting":"Word highlighting");
    if(!m_paused)m_player.play();updateHighlight(0);requestNext();
}
void MainWindow::nextSection(){if(!m_active)return;++m_current;if(m_current>=m_segments.size()){finishSpeech();return;}playCurrent();}
void MainWindow::pauseSpeech()
{
    if(!m_active)return;m_paused=!m_paused;if(m_paused)m_player.pause();else if(!m_audioPaths.value(m_current).isEmpty())m_player.play();m_pause->setText(m_paused?"Resume":"Pause");m_status->setText(QString(m_paused?"Paused · part %1 of %2":"Reading part %1 of %2").arg(m_current+1).arg(m_segments.size()));
}
void MainWindow::stopSpeech()
{
    ++m_playbackGeneration;m_active=false;m_detecting=false;m_tts.cancel();m_player.stop();m_requestBusy=false;m_request=-1;m_paused=false;m_text->clearReadingRange();setSpeechActive(false);m_status->setText("Playback stopped.");m_highlightInfo->setText("Playback highlighting on");
}
void MainWindow::finishSpeech()
{
    m_cacheComplete=true;for(const auto &p:m_audioPaths)if(p.isEmpty())m_cacheComplete=false;
    m_active=false;m_requestBusy=false;m_paused=false;m_text->clearReadingRange();setSpeechActive(false);m_export->setEnabled(m_cacheComplete);m_status->setText("Playback complete. Generated audio is available to save until you start a new reading or close the app.");m_highlightInfo->setText("Playback complete");
}
void MainWindow::speechError(const QString &message){if(!m_active)return;stopSpeech();m_status->setText(message);m_status->setToolTip(message);}
void MainWindow::updateHighlight(qint64 position)
{
    if(!m_active||m_current<0||m_current>=m_segments.size()||m_audioPaths.value(m_current).isEmpty()||!m_settings.value("highlight").toBool(true)){m_text->clearReadingRange();return;}
    const auto &s=m_segments[m_current];int word=-1,len=0;for(const auto &w:m_cues[m_current])if(position>=w.startMs&&position<w.endMs){word=s.start+w.start;len=w.length;break;}
    m_text->setReadingRange(s.start,s.length,word,len,m_settings.value("follow").toBool(true));
}
bool MainWindow::confirmReplace(NumberedEditor *editor,const QString &title){return editor->toPlainText().trimmed().isEmpty()||QMessageBox::question(this,title,"Replace the text currently in this editor?")==QMessageBox::Yes;}
void MainWindow::openText()
{
    if(m_active||m_dictationActive||m_sttBusy||m_recording)return;QString path=QFileDialog::getOpenFileName(this,"Open text",{},"Text files (*.txt *.md);;All files (*)");if(path.isEmpty())return;QFile f(path);if(!f.open(QIODevice::ReadOnly)||f.size()>2*1024*1024){m_status->setText("Could not open this text file (maximum 2 MiB).");return;}auto bytes=f.readAll();if(!confirmReplace(m_text,"Open text"))return;m_text->setPlainText(QString::fromUtf8(bytes));m_text->document()->setModified(false);
}
void MainWindow::saveAudio()
{
    if(!m_cacheComplete||m_merger.busy()||m_dictationActive||m_sttBusy)return;QString path=QFileDialog::getSaveFileName(this,"Save complete speech audio","speech.wav","WAV audio (*.wav)");if(path.isEmpty())return;if(!path.endsWith(".wav",Qt::CaseInsensitive))path+=".wav";QStringList paths;for(auto p:m_audioPaths)paths<<p;m_speak->setEnabled(false);m_merger.start(paths,path);updateSttControls();
}
void MainWindow::refreshDictationModels()
{
    QString selected=m_sttModel->currentData().toString();if(selected.isEmpty())selected=m_settings.value("sttModel").toString("whistle");
    QSignalBlocker blocker(m_sttModel);m_sttModel->clear();
    m_sttModel->addItem("Whistle · compact","whistle");m_sttModel->addItem("Whisper Tiny · Q5","tiny");m_sttModel->addItem("Whisper Base · Q5","base");m_sttModel->addItem("Whisper Small · Q5","small");
    for(const auto &model:ModelPacks::installedModels())m_sttModel->addItem(model.name,model.id);
    m_sttModel->setCurrentIndex(qMax(0,m_sttModel->findData(selected)));
}
void MainWindow::updateLanguages()
{
    QString selected=m_language->currentData().toString();if(selected.isEmpty())selected=m_settings.value("sttLanguage").toString("auto");m_language->clear();m_language->addItem("Automatic","auto");
    const QList<QPair<QString,QString>> common={{"English","en"},{"German","de"},{"French","fr"},{"Spanish","es"},{"Italian","it"},{"Dutch","nl"},{"Polish","pl"}};for(auto v:common)m_language->addItem(v.first,v.second);
    if(m_sttModel->currentData().toString()!="whistle")for(auto v:QList<QPair<QString,QString>>{{"Hungarian","hu"},{"Portuguese","pt"},{"Russian","ru"},{"Ukrainian","uk"},{"Chinese","zh"},{"Japanese","ja"},{"Korean","ko"},{"Arabic","ar"},{"Czech","cs"},{"Slovak","sk"},{"Romanian","ro"},{"Turkish","tr"},{"Swedish","sv"},{"Danish","da"},{"Finnish","fi"},{"Norwegian","no"},{"Greek","el"},{"Hindi","hi"}})m_language->addItem(v.first,v.second);
    m_language->setCurrentIndex(qMax(0,m_language->findData(selected)));
}
void MainWindow::updateMeterVisibility()
{
    const bool visible=m_dictate->isChecked();m_level->setVisible(visible);m_recordTime->setVisible(visible);
    if(auto layout=qobject_cast<QGridLayout *>(m_status->parentWidget()->layout())) {
        layout->removeWidget(m_status);layout->addWidget(m_status,0,0,1,visible?1:3);
    }
}
void MainWindow::updateSttControls()
{
    const bool busy=m_dictationActive||m_sttBusy||m_recording;
    m_record->setText(m_recording?"Stop recording":"Record");m_record->setEnabled(!m_active&&!m_merger.busy()&&(!busy||m_recording));
    m_transcribeFile->setEnabled(!busy&&!m_active&&!m_merger.busy());m_cancelStt->setEnabled(busy);
    m_sttModel->setEnabled(!busy);m_language->setEnabled(!busy);m_device->setEnabled(!busy);m_live->setEnabled(!busy);m_refreshDevices->setEnabled(!busy);
    m_dictate->setEnabled(!busy);m_text->setReadOnly(m_active||busy);m_openText->setEnabled(!m_active&&!busy);m_clearText->setEnabled(!m_active&&!busy);
    m_saveText->setEnabled(!busy&&!m_text->toPlainText().isEmpty());
    m_speak->setEnabled(!m_active&&!busy&&!m_merger.busy()&&!m_text->toPlainText().trimmed().isEmpty());
    m_export->setEnabled(m_cacheComplete&&!busy&&!m_merger.busy());
    if(!m_recording)m_level->setValue(0);
}
bool MainWindow::beginDictation()
{
    if(m_active||m_dictationActive||m_sttBusy||m_recording||m_merger.busy())return false;
    m_dictationOriginalCursor=m_text->textCursor();m_dictationReversed=m_dictationOriginalCursor.anchor()>m_dictationOriginalCursor.position();
    m_dictationStart=m_dictationOriginalCursor.selectionStart();m_dictationLength=m_dictationOriginalCursor.selectionEnd()-m_dictationStart;
    m_dictationOriginal=m_dictationOriginalCursor.selectedText().replace(QChar::ParagraphSeparator,'\n');
    const QString text=m_text->toPlainText();
    m_dictationBefore=m_dictationStart>0?text.at(m_dictationStart-1):QChar();
    const int end=m_dictationStart+m_dictationLength;m_dictationAfter=end<text.size()?text.at(end):QChar();
    m_dictationWasModified=m_text->document()->isModified();m_dictationChanged=false;m_dictationFinal=false;
    m_dictationActive=true;m_sttBusy=true;m_recordTime->setText("0:00");m_text->clearReadingRange();
    // Keep the insertion position fixed until this recognition task is complete.
    m_dictate->setChecked(true);m_sttStatus->setToolTip(QString());updateSttControls();return true;
}
void MainWindow::acceptDictation(const QString &recognized,bool final)
{
    if(!m_dictationActive||m_dictationFinal)return;
    QString text=recognized.trimmed();
    if(text.isEmpty()){
        if(final){finishDictation(false);m_sttStatus->setText("No speech was recognized. Your original text is unchanged.");}
        return;
    }
    // Space adjoining words without modifying anything outside the insertion.
    const bool previousNeedsSpace=m_dictationBefore.isLetterOrNumber()||QString(".,!?;:)]}\"'").contains(m_dictationBefore);
    if(!m_dictationBefore.isNull()&&!m_dictationBefore.isSpace()&&previousNeedsSpace&&text.front().isLetterOrNumber())text.prepend(' ');
    if(!m_dictationAfter.isNull()&&!m_dictationAfter.isSpace()&&m_dictationAfter.isLetterOrNumber()&&!text.back().isSpace())text.append(' ');
    QTextCursor cursor(m_text->document());cursor.setPosition(m_dictationStart);cursor.setPosition(m_dictationStart+m_dictationLength,QTextCursor::KeepAnchor);
    if(m_dictationChanged)cursor.joinPreviousEditBlock();else cursor.beginEditBlock();
    cursor.insertText(text);cursor.endEditBlock();m_dictationLength=text.size();m_dictationChanged=true;m_dictationFinal=final;
    m_text->setTextCursor(cursor);m_text->ensureCursorVisible();
    if(final)m_sttStatus->setText("Dictation inserted. Edit the text, then press Speak.");
    else m_sttStatus->setText("Listening · live text is provisional. Stop to finish; Cancel to discard.");
}
void MainWindow::finishDictation(bool keepText)
{
    if(!m_dictationActive)return;
    if(!keepText){
        if(m_dictationChanged){
            QTextCursor cursor(m_text->document());cursor.setPosition(m_dictationStart);cursor.setPosition(m_dictationStart+m_dictationLength,QTextCursor::KeepAnchor);
            cursor.joinPreviousEditBlock();cursor.insertText(m_dictationOriginal);cursor.endEditBlock();
        }
        // Stored cursor offsets can move during edits. Reconstruct the original
        // selection explicitly, including a selection made right-to-left.
        QTextCursor cursor(m_text->document());
        const int end=m_dictationStart+m_dictationOriginal.size();
        const bool reversed=m_dictationReversed;
        cursor.setPosition(reversed?end:m_dictationStart);cursor.setPosition(reversed?m_dictationStart:end,QTextCursor::KeepAnchor);
        m_text->setTextCursor(cursor);m_text->document()->setModified(m_dictationWasModified);
    }
    m_dictationActive=false;m_sttBusy=false;m_recording=false;updateSttControls();m_text->setFocus();
    m_speak->setText(m_text->textCursor().hasSelection()?"Speak selection":"Speak");
}
void MainWindow::cancelDictation()
{
    if(!m_dictationActive&&!m_sttBusy&&!m_recording)return;
    finishDictation(false);m_stt.cancel();m_sttBusy=false;m_recording=false;updateSttControls();
    m_sttStatus->setText("Dictation cancelled. Your original text is unchanged.");
}
void MainWindow::record()
{
    if(m_recording){m_record->setEnabled(false);m_stt.stopRecording();return;}
    if(!beginDictation())return;
    m_stt.setOptions(m_sttModel->currentData().toString(),m_language->currentData().toString(),m_settings.value("threads").toInt(4));
    m_stt.startRecording(m_device->currentData().toString(),m_live->isChecked());
}
void MainWindow::openAudio()
{
    if(m_active||m_dictationActive||m_sttBusy||m_recording||m_merger.busy())return;
    QString path=QFileDialog::getOpenFileName(this,"Insert text from audio",{},"Audio files (*.wav *.mp3 *.flac)");if(path.isEmpty()||!beginDictation())return;
    m_stt.setOptions(m_sttModel->currentData().toString(),m_language->currentData().toString(),m_settings.value("threads").toInt(4));m_stt.transcribeFile(path);
}
void MainWindow::saveText()
{
    if(m_dictationActive||m_sttBusy||m_recording||m_text->toPlainText().isEmpty())return;
    QString path=QFileDialog::getSaveFileName(this,"Save text","speech.txt","Text files (*.txt)");if(path.isEmpty())return;if(!path.endsWith(".txt",Qt::CaseInsensitive))path+=".txt";
    QSaveFile file(path);const auto bytes=m_text->toPlainText().toUtf8();
    if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())m_status->setText("Could not save text: "+file.errorString());
    else{m_status->setText("Text saved: "+QFileInfo(path).fileName());m_text->document()->setModified(false);}
}
void MainWindow::configure(bool serverPage)
{
    if(m_active||m_sttBusy||m_merger.busy()){QMessageBox::information(this,"Settings","Stop the active playback or dictation task before changing settings.");return;}
    rememberActiveProfile();SettingsDialog d(m_settings,this,QDir(configDirectory()).filePath("credentials"));
    d.setSaveHandler([this](const QJsonObject &settings,QString *error){return writeSettings(settings,error);});
    connect(&d,&SettingsDialog::credentialLocked,this,&MainWindow::lockProfileCredential);
    if(serverPage)d.showServers();
    if(d.exec()==QDialog::Accepted){m_settings=d.resultSettings();m_tts.setProfile({});updateProfileChoices();m_lastTheme.clear();applyTheme();}
    else updateVoiceChoices();
    // Installing a pack is an explicit independent action even if the user
    // dismisses the remaining preferences with Cancel.
    refreshDictationModels();updateLanguages();
}
bool MainWindow::systemDark() const
{
#ifdef Q_OS_WIN
    QSettings s("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",QSettings::NativeFormat);return s.value("AppsUseLightTheme",1).toInt()==0;
#else
    QDBusMessage msg=QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop","/org/freedesktop/portal/desktop","org.freedesktop.portal.Settings","Read");msg<<"org.freedesktop.appearance"<<"color-scheme";
    auto reply=QDBusConnection::sessionBus().call(msg,QDBus::Block,150);if(reply.type()==QDBusMessage::ReplyMessage&&!reply.arguments().isEmpty()){QVariant v=reply.arguments().first();if(v.canConvert<QDBusVariant>())v=v.value<QDBusVariant>().variant();if(v.canConvert<QDBusVariant>())v=v.value<QDBusVariant>().variant();int value=v.toInt();if(value==1)return true;if(value==2)return false;}
    return qEnvironmentVariable("GTK_THEME").contains("dark",Qt::CaseInsensitive);
#endif
}
void MainWindow::applyTheme()
{
    QString mode=m_settings.value("theme").toString("system");bool dark=mode=="dark"||(mode=="system"&&systemDark());QString key=QString::number(dark)+":"+QString::number(m_settings.value("fontSize").toInt(16));if(m_lastTheme==key)return;m_lastTheme=key;m_dark=dark;
    const bool textWasModified=m_text->document()->isModified();
    const QString bg=dark?"#191a1d":"#f3f4f5",panel=dark?"#222326":"#ffffff",input=dark?"#292a2e":"#ffffff",text=dark?"#f0f0f2":"#1e2025",muted=dark?"#a5a7af":"#646974",border=dark?"#37393f":"#dcdfe4",hover=dark?"#34363c":"#eceff3";
    const QString accent=dark?"#e2e5e9":"#30343a",accentText=dark?"#1d2024":"#ffffff",accentHover=dark?"#f2f4f6":"#454b53",focus=dark?"#9299a3":"#6b737e",selection=dark?"#444a53":"#cdd3db";
    QPalette p;p.setColor(QPalette::Window,QColor(bg));p.setColor(QPalette::WindowText,QColor(text));p.setColor(QPalette::Base,QColor(panel));p.setColor(QPalette::AlternateBase,QColor(dark?"#202124":"#f7f8fa"));p.setColor(QPalette::Text,QColor(text));p.setColor(QPalette::Button,QColor(input));p.setColor(QPalette::ButtonText,QColor(text));p.setColor(QPalette::Highlight,QColor(selection));p.setColor(QPalette::HighlightedText,QColor(text));p.setColor(QPalette::PlaceholderText,QColor(muted));p.setColor(QPalette::ToolTipBase,QColor(panel));p.setColor(QPalette::ToolTipText,QColor(text));p.setColor(QPalette::Disabled,QPalette::Text,QColor(muted));p.setColor(QPalette::Disabled,QPalette::ButtonText,QColor(dark?"#777b83":"#999da6"));qApp->setPalette(p);
    QString css=QString(R"(
QWidget { color:%1; font-size:13px; }
QMainWindow, QDialog { background:%2; }
QLabel[muted="true"] { color:%3; font-size:12px; }
QLabel#brand { font-size:23px; font-weight:650; }
QLabel[heading="true"] { font-size:22px; font-weight:600; }
QLabel[sectionLabel="true"] { font-size:11px; font-weight:600; letter-spacing:1px; color:%3; }
QFrame#card { background:%4; border:1px solid %5; border-radius:10px; }
QTabWidget::pane { border:0; }
QTabBar::tab { background:transparent; color:%3; padding:11px 18px; margin-right:10px; border-bottom:2px solid transparent; }
QTabBar::tab:selected { color:%1; font-weight:600; border-bottom:2px solid %8; }
QTabBar::tab:hover { color:%1; }
QPushButton,QToolButton { background:%6; border:1px solid %5; border-radius:7px; padding:8px 13px; }
QToolButton { padding:5px 9px; background:transparent; border:0; }
QPushButton:hover,QToolButton:hover { background:%7; }
QToolButton:checked { background:%7; color:%8; }
QPushButton:pressed,QToolButton:pressed { background:%5; }
QPushButton:checked { background:%5; border-color:%3; font-weight:600; }
QPushButton[accent="true"] { background:%8; border-color:%8; color:%9; font-weight:600; }
QPushButton[accent="true"]:hover { background:%10; }
QPushButton:disabled,QToolButton:disabled { color:%3; background:transparent; }
QPushButton:focus,QComboBox:focus,QLineEdit:focus,QSpinBox:focus { border-color:%11; }
QComboBox,QLineEdit,QSpinBox { background:%6; border:1px solid %5; border-radius:6px; padding:7px 9px; min-height:18px; }
QComboBox { padding-right:24px; }
QComboBox::drop-down { border:0; width:23px; }
QComboBox::down-arrow { image:url(:/chevron.svg); width:12px; height:8px; }
QComboBox QAbstractItemView { background:%4; border:1px solid %5; selection-background-color:%12; selection-color:%1; }
QPlainTextEdit { background:%4; color:%1; border:0; selection-background-color:%12; selection-color:%1; }
QScrollBar:vertical { background:transparent; width:10px; margin:2px; }
QScrollBar::handle:vertical { background:%5; border-radius:4px; min-height:28px; }
QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }
QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical { background:transparent; }
QSlider::groove:horizontal { height:4px; background:%5; border-radius:2px; }
QSlider::sub-page:horizontal { background:%8; border-radius:2px; }
QSlider::handle:horizontal { background:%8; width:12px; margin:-4px 0; border-radius:6px; }
QProgressBar { border:0; background:%5; border-radius:3px; }
QProgressBar::chunk { background:#4b9d7a; border-radius:3px; }
QCheckBox { spacing:8px; }
QCheckBox::indicator { width:16px; height:16px; border:1px solid %5; border-radius:4px; background:%6; }
QCheckBox::indicator:checked { background:#737b87; image:url(:/check.svg); border-color:#737b87; }
QToolTip { background:%4; border:1px solid %5; padding:6px; }
)").arg(text,bg,muted,panel,border,input,hover).arg(accent,accentText,accentHover,focus,selection);
    css+=QString("QFrame#dictationInset { background:%1; border:1px solid %2; border-top-color:%3; border-left-color:%3; border-radius:8px; }")
        .arg(dark?"#1b1c1f":"#eceeef",dark?"#484a50":"#ffffff",dark?"#101114":"#c5c9d0");
    qApp->setStyleSheet(css);m_level->setDark(dark);m_live->setDark(dark);QFont editorFont=qApp->font();editorFont.setPixelSize(m_settings.value("fontSize").toInt(16));m_text->setFont(editorFont);
    m_text->setStyleSheet(QString("font-size:%1px;").arg(editorFont.pixelSize()));
    // Appearance changes must neither create nor erase an unsaved-text state.
    m_text->document()->setModified(textWasModified);
}
void MainWindow::setThemeOverride(const QString &theme){m_settings["theme"]=theme;m_lastTheme.clear();applyTheme();}
void MainWindow::showStt(){showDictation();}
void MainWindow::showDictation(){m_dictate->setChecked(true);}
void MainWindow::showDemo(bool dictation)
{
    m_demo=true;
    const QString example="A good voice makes room for the words.\n\nRead a thought aloud, listen to its rhythm, and give it time to land. SpeechDesk brings clear playback and focused writing into one simple workspace.\n\nType, paste, open a text file, or dictate a new idea. Your words arrive in this editor, ready to refine and read aloud.";
    m_text->setPlainText(example);
    if(dictation){
        // Preview opens the real controls without starting a microphone or inference.
        QSignalBlocker blocked(m_dictate);m_dictate->setChecked(true);m_dictationPanel->show();updateMeterVisibility();
        m_text->moveCursor(QTextCursor::End);m_status->setText("Preview · sample text with dictation controls open. No recording is active.");
    }else{
        int start=example.indexOf("Read a thought");int end=example.indexOf("SpeechDesk");m_text->setReadingRange(start,end-start,start,4,false);m_highlightInfo->setText("Highlighting preview");m_status->setText("Preview · sample text and playback highlight");
    }
    m_text->document()->setModified(false);
}
void MainWindow::closeEvent(QCloseEvent *event)
{
    if((m_active||m_recording||m_sttBusy||m_merger.busy())&&QMessageBox::question(this,"Close SpeechDesk","Stop the active task and close SpeechDesk?")!=QMessageBox::Yes){event->ignore();return;}
    if(!m_demo&&m_text->document()->isModified()&&QMessageBox::question(this,"Unsaved text","Text in the editor is not saved automatically. Close without saving it?")!=QMessageBox::Yes){event->ignore();return;}
    saveSettings();m_active=false;m_tts.cancel();m_stt.cancel();m_merger.cancel();m_player.stop();m_player.setSource(QUrl());event->accept();
}

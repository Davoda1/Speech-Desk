#pragma once
#include <QMainWindow>
#include <QJsonObject>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTextCursor>
#include <QDate>
#include <memory>
#include "numberededitor.h"
#include "speechtypes.h"
#include "ttsclient.h"
#include "sttservice.h"
#include "audiomerger.h"
#include "speechwidgets.h"
class MainWindow:public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent=nullptr);
    void setThemeOverride(const QString &theme);
    void showDemo(bool dictation=false);
    void showStt();
    void showDictation();
    void configure(bool serverPage=false);
    static QString configDirectory();
protected:
    void closeEvent(QCloseEvent *) override;
private:
    QJsonObject m_settings;
    NumberedEditor *m_text;
    QWidget *m_dictationPanel;
    QToolButton *m_dictate,*m_openText,*m_clearText,*m_saveText,*m_refreshDevices;
    QComboBox *m_server,*m_model,*m_voice,*m_sttModel,*m_language,*m_device;
    QPushButton *m_speak,*m_pause,*m_stop,*m_record,*m_transcribeFile,*m_cancelStt,*m_export;
    QSlider *m_seek,*m_volume;
    SymmetricLevelMeter *m_level;
    AnimatedToggleButton *m_live;
    QLabel *m_status,*m_sttStatus,*m_stats,*m_time,*m_highlightInfo,*m_recordTime;
    TtsClient m_tts;
    SttService m_stt;
    AudioMerger m_merger;
    QMediaPlayer m_player;
    QAudioOutput m_output;
    std::unique_ptr<QTemporaryDir> m_audioDir;
    QVector<SpeechSegment> m_segments;
    QVector<QString> m_audioPaths,m_audioFormats;
    QVector<QVector<WordCue>> m_cues;
    int m_current=-1,m_request=-1;
    quint64 m_playbackGeneration=0;
    bool m_active=false,m_requestBusy=false,m_paused=false,m_cacheComplete=false,m_recording=false,m_dark=false,m_sttBusy=false;
    bool m_demo=false,m_detecting=false;
    bool m_dictationActive=false,m_dictationChanged=false,m_dictationFinal=false,m_dictationWasModified=false,m_dictationReversed=false;
    int m_dictationStart=0,m_dictationLength=0;
    QString m_dictationOriginal;
    QTextCursor m_dictationOriginalCursor;
    QChar m_dictationBefore,m_dictationAfter;
    QString m_lastTheme,m_cachedText;
    QDate m_lifecycleDate;
    QByteArray m_cachedProfile;
    QWidget *buildTts();
    QWidget *buildDictation();
    void loadSettings();
    void saveSettings();
    bool writeSettings(const QJsonObject &settings,QString *error);
    bool unlockSelectedCredential();
    void lockProfileCredential(const QString &id);
    void applyTheme();
    bool systemDark() const;
    QJsonObject selectedProfile() const;
    void updateProfileChoices();
    void updateVoiceChoices();
    void updateModelVoices(bool modelChanged = false);
    void rememberActiveProfile();
    void updateStatistics();
    void startSpeech();
    void pauseSpeech();
    void stopSpeech();
    void requestNext();
    void playCurrent();
    void nextSection();
    void updateHighlight(qint64 position);
    void finishSpeech();
    void setSpeechActive(bool active);
    void speechError(const QString &message);
    void openText();
    void saveAudio();
    void updateSttControls();
    void updateMeterVisibility();
    void updateLanguages();
    void refreshDictationModels();
    void record();
    void openAudio();
    void saveText();
    bool beginDictation();
    void acceptDictation(const QString &text,bool final);
    void finishDictation(bool keepText);
    void cancelDictation();
    bool confirmReplace(NumberedEditor *editor,const QString &title);
};

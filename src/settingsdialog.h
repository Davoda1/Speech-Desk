#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QJsonArray>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include "ttsclient.h"
QJsonObject defaultServerProfile();
QJsonObject serverProfileForProvider(const QString &provider);
QJsonObject publicServerProfile(QJsonObject profile);
class SettingsDialog:public QDialog {
    Q_OBJECT
public:
    SettingsDialog(const QJsonObject &settings,QWidget *parent=nullptr);
    QJsonObject resultSettings();
    void showServers();
private:
    QJsonObject m_settings;
    QJsonArray m_profiles;
    int m_current=-1;
    class QTabWidget *m_tabs;
    QComboBox *m_theme,*m_profilesBox,*m_provider,*m_adapter;
    QLineEdit *m_name,*m_url,*m_key,*m_model,*m_voice;
    QSpinBox *m_fontSize,*m_threads,*m_chunkSize,*m_timeout;
    QCheckBox *m_follow,*m_highlight;
    QLabel *m_connectionInfo,*m_modelPackInfo,*m_providerInfo;
    TtsClient m_probe;
    void loadProfile(int index);
    void storeProfile();
    void applyProvider(int index);
    void updateProviderInfo();
    void refreshProfiles(int index);
    void editAdvanced();
    void importProfile();
    void exportProfile();
    void detect();
    void importModelPack();
    void refreshModelPacks();
};

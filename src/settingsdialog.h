#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QJsonArray>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <QMap>
#include <QSet>
#include <functional>
#include "credentialstore.h"
#include "ttsclient.h"
QJsonObject defaultServerProfile();
QJsonObject serverProfileForProvider(const QString &provider);
QJsonObject publicServerProfile(QJsonObject profile);
QJsonObject currentServerProfile(QJsonObject profile);
class SettingsDialog:public QDialog {
    Q_OBJECT
public:
    SettingsDialog(const QJsonObject &settings,QWidget *parent=nullptr,const QString &credentialDirectory={});
    QJsonObject resultSettings();
    void showServers();
    void setSaveHandler(std::function<bool(const QJsonObject &,QString *)> handler);
    bool commitChanges(QString *error=nullptr);
signals:
    void credentialLocked(const QString &profileId);
private:
    QJsonObject m_settings;
    QJsonArray m_profiles;
    int m_current=-1;
    class QTabWidget *m_tabs;
    QComboBox *m_theme,*m_profilesBox,*m_provider,*m_adapter;
    QLineEdit *m_name,*m_url,*m_key,*m_model,*m_voice;
    QSpinBox *m_fontSize,*m_threads,*m_chunkSize,*m_timeout;
    QCheckBox *m_follow,*m_highlight,*m_saveKey;
    QLabel *m_connectionInfo,*m_modelPackInfo,*m_providerInfo,*m_credentialInfo,*m_lifecycleInfo;
    class QPushButton *m_unlockKey,*m_lockKey,*m_replaceKey;
    QString m_credentialDirectory;
    CredentialStore m_credentials;
    QMap<QString,QByteArray> m_pendingCredentials;
    QSet<QString> m_pendingCredentialRemovals;
    std::function<bool(const QJsonObject &,QString *)> m_saveHandler;
    TtsClient m_probe;
    void loadProfile(int index);
    void storeProfile();
    void applyProvider(int index);
    void updateProviderInfo();
    bool hasSavedCredential(const QString &id) const;
    void refreshCredentialControls();
    void stageCredentialRemoval(const QString &id);
    bool unlockCurrentCredential();
    void protectCurrentCredential(bool replacement);
    void lockCurrentCredential();
    void saveAndAccept();
    void refreshProfiles(int index);
    void editAdvanced();
    void importProfile();
    void exportProfile();
    void detect();
    void importModelPack();
    void refreshModelPacks();
};

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// One independently encrypted credential per profile. This class retains no
// unlocked credentials or passphrases and has no implicit filesystem writes.
class CredentialStore final {
public:
    struct Snapshot {
        bool existed = false;
        QByteArray bytes;
    };

    explicit CredentialStore(const QString &directory);

    bool exists(const QString &profileId) const;
    bool read(const QString &profileId, QByteArray *encrypted,
              QString *error = nullptr) const;
    bool commit(const QString &profileId, const QByteArray &encrypted,
                QString *error = nullptr);
    bool remove(const QString &profileId, QString *error = nullptr);
    // For Save rollback only: preserve an existing bounded opaque file even if
    // its encrypted header is damaged. Restore exact bytes with restrictive
    // permissions, or remove a new record when the snapshot captured absence.
    // Never use restoreSnapshot() to save a newly supplied API key.
    bool snapshot(const QString &profileId, Snapshot *state,
                  QString *error = nullptr) const;
    bool restoreSnapshot(const QString &profileId, const Snapshot &state,
                         QString *error = nullptr);
    bool unlock(const QJsonObject &profile, const QByteArray &passphrase,
                QByteArray *apiKey, QString *error = nullptr) const;

    // These operations only produce/consume bytes. Keep encrypted bytes staged
    // until Settings Save; discarding them on Cancel does not change any file.
    // The binding is profile id + provider + normalized HTTP(S) origin.
    // Each KDF uses 256 MiB, so callers should run it away from the GUI thread.
    static QByteArray encrypt(const QJsonObject &profile, const QByteArray &apiKey,
                              const QByteArray &passphrase, QString *error = nullptr);
    static bool decrypt(const QJsonObject &profile, const QByteArray &encrypted,
                        const QByteArray &passphrase, QByteArray *apiKey,
                        QString *error = nullptr);
    static QString normalizedOrigin(const QString &url);

    // Best effort for an owned byte array. This cannot erase copies retained by
    // callers, Qt strings, network requests, or the operating system.
    static void wipe(QByteArray &bytes);

private:
    QString m_directory;
    QString pathFor(const QString &profileId) const;
    bool usableDirectory(bool create, QString *error) const;
    bool writeBytes(const QString &profileId, const QByteArray &bytes, QString *error);
};

#include "credentialstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <QtEndian>
#include <array>
#include <limits>
#include <sodium.h>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <io.h>
#endif

namespace {
constexpr char Magic[] = {'S', 'D', 'K', 'E', 'Y', '0', '0', '1'};
constexpr quint32 FormatVersion = 1;
constexpr quint32 KdfArgon2id13 = 1;
// Version 1 pins the serialized parameters; never trust arbitrary file-supplied
// work factors or silently reduce them when memory allocation fails.
constexpr quint64 KdfOps = 3;
constexpr quint64 KdfMemory = 256ULL * 1024ULL * 1024ULL;
constexpr qsizetype MaximumKeyBytes = 16384;
constexpr qsizetype MaximumPassphraseBytes = 4096;
constexpr qsizetype SaltOffset = 32;
constexpr qsizetype NonceOffset = SaltOffset + crypto_pwhash_SALTBYTES;
constexpr qsizetype LengthOffset = NonceOffset + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr qsizetype HeaderBytes = LengthOffset + 4;
constexpr qsizetype MaximumEnvelopeBytes = HeaderBytes + MaximumKeyBytes + crypto_aead_xchacha20poly1305_ietf_ABYTES;
constexpr qsizetype MaximumSnapshotBytes = 64 * 1024;
static_assert(KdfOps == crypto_pwhash_OPSLIMIT_MODERATE);
static_assert(KdfMemory == crypto_pwhash_MEMLIMIT_MODERATE);
static_assert(crypto_pwhash_SALTBYTES == 16);
static_assert(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES == 24);

void setError(QString *error, const QString &message)
{
    if (error) *error = message;
}

bool fail(QString *error, const char *message)
{
    setError(error, QString::fromLatin1(message));
    return false;
}

bool sodiumReady(QString *error)
{
    static const bool ready = sodium_init() >= 0;
    return ready || fail(error, "Encryption is unavailable. No credential was saved.");
}

QString canonicalId(const QString &id)
{
    const QUuid uuid(id);
    return uuid.isNull() ? QString() : uuid.toString(QUuid::WithoutBraces);
}

void append32(QByteArray &bytes, quint32 value)
{
    const auto networkValue = qToBigEndian(value);
    bytes.append(reinterpret_cast<const char *>(&networkValue), sizeof(networkValue));
}

void append64(QByteArray &bytes, quint64 value)
{
    const auto networkValue = qToBigEndian(value);
    bytes.append(reinterpret_cast<const char *>(&networkValue), sizeof(networkValue));
}

void appendField(QByteArray &bytes, const QByteArray &value)
{
    append32(bytes, static_cast<quint32>(value.size()));
    bytes.append(value);
}

QByteArray bindingFor(const QJsonObject &profile, QString *error)
{
    const auto id = canonicalId(profile.value("id").toString());
    const auto provider = profile.value("provider").toString("selfhosted").trimmed().toLower();
    const auto origin = CredentialStore::normalizedOrigin(profile.value("baseUrl").toString());
    static const QRegularExpression validProvider(QStringLiteral("^[a-z0-9][a-z0-9_-]{0,63}$"));
    if (id.isEmpty() || origin.isEmpty() || !validProvider.match(provider).hasMatch()) {
        fail(error, "The profile needs a valid identifier, provider and HTTP(S) server address before a key can be saved or unlocked.");
        return {};
    }
    QByteArray binding("SpeechDesk credential binding v1");
    appendField(binding, id.toUtf8());
    appendField(binding, provider.toUtf8());
    appendField(binding, origin.toUtf8());
    return binding;
}

bool validPassphrase(const QByteArray &passphrase, QString *error)
{
    return (!passphrase.isEmpty() && passphrase.size() <= MaximumPassphraseBytes)
        || fail(error, "Enter a nonempty passphrase of at most 4096 UTF-8 bytes.");
}

bool validEnvelope(const QByteArray &bytes, QString *error)
{
    if (bytes.size() < HeaderBytes + crypto_aead_xchacha20poly1305_ietf_ABYTES + 1
        || bytes.size() > MaximumEnvelopeBytes)
        return fail(error, "The encrypted credential file is damaged or unsupported.");
    if (bytes.first(sizeof(Magic)) != QByteArray(Magic, sizeof(Magic))
        || qFromBigEndian<quint32>(bytes.constData() + 8) != FormatVersion
        || qFromBigEndian<quint32>(bytes.constData() + 12) != KdfArgon2id13
        || qFromBigEndian<quint64>(bytes.constData() + 16) != KdfOps
        || qFromBigEndian<quint64>(bytes.constData() + 24) != KdfMemory)
        return fail(error, "The encrypted credential format or encryption parameters are unsupported.");
    const auto size = qFromBigEndian<quint32>(bytes.constData() + LengthOffset);
    return size == static_cast<quint32>(bytes.size() - HeaderBytes)
        || fail(error, "The encrypted credential file is damaged or unsupported.");
}

class DerivedKey final {
public:
    DerivedKey() { locked = sodium_mlock(bytes.data(), bytes.size()) == 0; }
    ~DerivedKey()
    {
        sodium_memzero(bytes.data(), bytes.size());
        if (locked) sodium_munlock(bytes.data(), bytes.size());
    }
    DerivedKey(const DerivedKey &) = delete;
    DerivedKey &operator=(const DerivedKey &) = delete;
    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_KEYBYTES> bytes{};
private:
    bool locked = false;
};

bool derive(DerivedKey &key, const QByteArray &passphrase,
            const unsigned char *salt, QString *error)
{
    // Bound peak KDF memory even when two independent UI tasks start together.
    static QMutex kdfMutex;
    const QMutexLocker lock(&kdfMutex);
    if (crypto_pwhash(key.bytes.data(), key.bytes.size(), passphrase.constData(),
                      static_cast<unsigned long long>(passphrase.size()), salt,
                      KdfOps, static_cast<size_t>(KdfMemory), crypto_pwhash_ALG_ARGON2ID13) != 0)
        return fail(error, "Could not derive the encryption key. About 256 MiB of available memory is required; no weaker encryption was used.");
    return true;
}

bool unsafeLink(const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink()) return true;
#ifdef Q_OS_WIN
    const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return false;
#endif
}

#ifdef Q_OS_WIN
// Protect files from inherited broad ACLs. The current account and SYSTEM retain
// full access. A privileged administrator can still take ownership of files.
bool restrictWindowsAccess(const QString &path, HANDLE file, bool directory)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (!size || size > 65536) { CloseHandle(token); return false; }
    QByteArray tokenBytes(static_cast<qsizetype>(size), Qt::Uninitialized);
    const bool gotUser = GetTokenInformation(token, TokenUser, tokenBytes.data(), size, &size);
    CloseHandle(token);
    if (!gotUser) return false;
    const auto *user = reinterpret_cast<const TOKEN_USER *>(tokenBytes.constData());
    alignas(DWORD) std::array<unsigned char, SECURITY_MAX_SID_SIZE> systemSid{};
    DWORD systemSize = static_cast<DWORD>(systemSid.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid.data(), &systemSize)) return false;
    EXPLICIT_ACCESSW access[2]{};
    for (auto &entry : access) {
        entry.grfAccessPermissions = FILE_ALL_ACCESS;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
    }
    access[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(user->User.Sid);
    access[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(systemSid.data());
    PACL acl = nullptr;
    if (SetEntriesInAclW(2, access, nullptr, &acl) != ERROR_SUCCESS) return false;
    constexpr auto flags = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
    const DWORD result = file != INVALID_HANDLE_VALUE
        ? SetSecurityInfo(file, SE_FILE_OBJECT, flags, nullptr, nullptr, acl, nullptr)
        : SetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
                                SE_FILE_OBJECT, flags, nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    return result == ERROR_SUCCESS;
}
#endif

bool restrictDirectory(const QString &directory)
{
#ifdef Q_OS_WIN
    return restrictWindowsAccess(directory, INVALID_HANDLE_VALUE, true);
#else
    return QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
}

bool restrictFile(QSaveFile &file)
{
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()));
    if (handle == INVALID_HANDLE_VALUE) return false;
    // Qt opens the data handle for writing, which does not necessarily grant
    // WRITE_DAC. Reopen the same temporary file with the permission-change
    // right instead of finding it by a guessed temporary filename.
    const HANDLE permissionsHandle = ReOpenFile(handle, WRITE_DAC | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (permissionsHandle == INVALID_HANDLE_VALUE) return false;
    const bool restricted = restrictWindowsAccess({}, permissionsHandle, false);
    CloseHandle(permissionsHandle);
    return restricted;
#else
    return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
}
}

CredentialStore::CredentialStore(const QString &directory)
    : m_directory(directory.isEmpty() ? QString() : QFileInfo(directory).absoluteFilePath())
{}

QString CredentialStore::pathFor(const QString &profileId) const
{
    const auto id = canonicalId(profileId);
    return id.isEmpty() || m_directory.isEmpty() ? QString() : QDir(m_directory).filePath(id + ".vault");
}

bool CredentialStore::usableDirectory(bool create, QString *error) const
{
    if (m_directory.isEmpty() || unsafeLink(m_directory))
        return fail(error, "The encrypted-credential folder is unavailable or is a symbolic link.");
    QFileInfo info(m_directory);
    if (!info.exists()) {
        if (!create) return fail(error, "No saved credential was found for this profile.");
        if (!QDir().mkpath(m_directory))
            return fail(error, "Could not create the encrypted-credential folder.");
        info.refresh();
    }
    if (!info.isDir() || unsafeLink(m_directory))
        return fail(error, "The encrypted-credential folder is not a regular directory.");
    if (create && !restrictDirectory(m_directory))
        return fail(error, "Could not restrict access to the encrypted-credential folder. No credential was saved.");
    return true;
}

bool CredentialStore::exists(const QString &profileId) const
{
    const auto path = pathFor(profileId);
    return !path.isEmpty() && !unsafeLink(m_directory) && !unsafeLink(path) && QFileInfo(path).isFile();
}

bool CredentialStore::read(const QString &profileId, QByteArray *encrypted, QString *error) const
{
    setError(error, {});
    if (!encrypted) return fail(error, "No credential output buffer was supplied.");
    encrypted->clear();
    const auto path = pathFor(profileId);
    if (path.isEmpty()) return fail(error, "The profile identifier is invalid.");
    if (!usableDirectory(false, error)) return false;
    const QFileInfo info(path);
    if (unsafeLink(path) || !info.isFile())
        return fail(error, "No regular saved credential file was found for this profile.");
    if (info.size() > MaximumEnvelopeBytes)
        return fail(error, "The encrypted credential file is too large.");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, "Could not read the encrypted credential file.");
    const auto bytes = file.read(MaximumEnvelopeBytes + 1);
    if (file.error() != QFileDevice::NoError || !file.atEnd() || !validEnvelope(bytes, error)) {
        if (error && error->isEmpty()) *error = QStringLiteral("Could not read a complete encrypted credential file.");
        return false;
    }
    *encrypted = bytes;
    return true;
}

bool CredentialStore::commit(const QString &profileId, const QByteArray &encrypted, QString *error)
{
    setError(error, {});
    if (!validEnvelope(encrypted, error)) return false;
    return writeBytes(profileId, encrypted, error);
}

bool CredentialStore::writeBytes(const QString &profileId, const QByteArray &bytes, QString *error)
{
    const auto path = pathFor(profileId);
    if (path.isEmpty()) return fail(error, "The profile identifier is invalid.");
    if (!usableDirectory(true, error)) return false;
    const QFileInfo existing(path);
    if (unsafeLink(path) || (existing.exists() && !existing.isFile()))
        return fail(error, "The saved credential path is not a regular file. The existing item was preserved.");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, "Could not prepare the encrypted credential file. The existing saved key was preserved.");
    if (!restrictFile(file)) {
        file.cancelWriting();
        return fail(error, "Could not restrict access to the encrypted credential file. The existing saved key was preserved.");
    }
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return fail(error, "Could not write the encrypted credential file. The existing saved key was preserved.");
    }
    if (!file.commit())
        return fail(error, "Could not replace the encrypted credential file. The existing saved key was preserved.");
    return true;
}

bool CredentialStore::snapshot(const QString &profileId, Snapshot *state, QString *error) const
{
    setError(error, {});
    if (!state) return fail(error, "No credential rollback buffer was supplied.");
    *state = {};
    const auto path = pathFor(profileId);
    if (path.isEmpty()) return fail(error, "The profile identifier is invalid.");
    const QFileInfo directory(m_directory);
    if (unsafeLink(m_directory) || (directory.exists() && !directory.isDir()))
        return fail(error, "The encrypted-credential folder is unavailable or is a symbolic link.");
    if (!directory.exists()) return true;
    const QFileInfo info(path);
    if (unsafeLink(path) || (info.exists() && !info.isFile()))
        return fail(error, "The saved credential path is not a regular file. It was preserved.");
    if (!info.exists()) return true;
    if (info.size() > MaximumSnapshotBytes)
        return fail(error, "The existing credential file exceeds the 64 KiB rollback limit. It was preserved; remove it explicitly before replacing it.");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, "Could not preserve the existing credential file before saving. It was not changed.");
    const auto bytes = file.read(MaximumSnapshotBytes + 1);
    if (file.error() != QFileDevice::NoError || !file.atEnd() || bytes.size() > MaximumSnapshotBytes)
        return fail(error, "Could not preserve a complete bounded copy of the existing credential file. It was not changed.");
    state->existed = true;
    state->bytes = bytes;
    return true;
}

bool CredentialStore::restoreSnapshot(const QString &profileId, const Snapshot &state, QString *error)
{
    setError(error, {});
    if (state.bytes.size() > MaximumSnapshotBytes || (!state.existed && !state.bytes.isEmpty()))
        return fail(error, "The credential rollback snapshot is invalid or too large.");
    return state.existed ? writeBytes(profileId, state.bytes, error) : remove(profileId, error);
}

bool CredentialStore::remove(const QString &profileId, QString *error)
{
    setError(error, {});
    const auto path = pathFor(profileId);
    if (path.isEmpty()) return fail(error, "The profile identifier is invalid.");
    if (unsafeLink(m_directory) || unsafeLink(path))
        return fail(error, "The credential path is a symbolic link; it was not removed.");
    const QFileInfo info(path);
    if (!info.exists()) return true;
    if (!info.isFile()) return fail(error, "The credential path is not a regular file; it was not removed.");
    return QFile::remove(path) || fail(error, "Could not remove the saved encrypted credential.");
}

bool CredentialStore::unlock(const QJsonObject &profile, const QByteArray &passphrase,
                             QByteArray *apiKey, QString *error) const
{
    if (apiKey) wipe(*apiKey);
    QByteArray encrypted;
    return read(profile.value("id").toString(), &encrypted, error)
        && decrypt(profile, encrypted, passphrase, apiKey, error);
}

QByteArray CredentialStore::encrypt(const QJsonObject &profile, const QByteArray &apiKey,
                                    const QByteArray &passphrase, QString *error)
{
    setError(error, {});
    if (!sodiumReady(error) || !validPassphrase(passphrase, error)) return {};
    if (apiKey.isEmpty() || apiKey.size() > MaximumKeyBytes) {
        fail(error, "The API key must contain between 1 and 16384 bytes.");
        return {};
    }
    const auto binding = bindingFor(profile, error);
    if (binding.isEmpty()) return {};
    QByteArray header(Magic, sizeof(Magic));
    append32(header, FormatVersion);
    append32(header, KdfArgon2id13);
    append64(header, KdfOps);
    append64(header, KdfMemory);
    header.resize(LengthOffset);
    randombytes_buf(header.data() + SaltOffset, crypto_pwhash_SALTBYTES);
    randombytes_buf(header.data() + NonceOffset, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    append32(header, static_cast<quint32>(apiKey.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES));
    DerivedKey key;
    if (!derive(key, passphrase, reinterpret_cast<const unsigned char *>(header.constData() + SaltOffset), error)) return {};
    const auto aad = header + binding;
    QByteArray encrypted(apiKey.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, Qt::Uninitialized);
    unsigned long long size = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char *>(encrypted.data()), &size,
            reinterpret_cast<const unsigned char *>(apiKey.constData()), static_cast<unsigned long long>(apiKey.size()),
            reinterpret_cast<const unsigned char *>(aad.constData()), static_cast<unsigned long long>(aad.size()), nullptr,
            reinterpret_cast<const unsigned char *>(header.constData() + NonceOffset), key.bytes.data()) != 0
        || size != static_cast<unsigned long long>(encrypted.size())) {
        wipe(encrypted);
        fail(error, "Could not encrypt the API key. No credential was saved.");
        return {};
    }
    return header + encrypted;
}

bool CredentialStore::decrypt(const QJsonObject &profile, const QByteArray &encrypted,
                              const QByteArray &passphrase, QByteArray *apiKey, QString *error)
{
    setError(error, {});
    if (!apiKey) return fail(error, "No credential output buffer was supplied.");
    wipe(*apiKey);
    if (!sodiumReady(error) || !validPassphrase(passphrase, error) || !validEnvelope(encrypted, error)) return false;
    const auto binding = bindingFor(profile, error);
    if (binding.isEmpty()) return false;
    DerivedKey key;
    if (!derive(key, passphrase, reinterpret_cast<const unsigned char *>(encrypted.constData() + SaltOffset), error)) return false;
    const auto aad = encrypted.first(HeaderBytes) + binding;
    QByteArray plain(encrypted.size() - HeaderBytes - crypto_aead_xchacha20poly1305_ietf_ABYTES, Qt::Uninitialized);
    unsigned long long size = 0;
    const int result = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char *>(plain.data()), &size, nullptr,
        reinterpret_cast<const unsigned char *>(encrypted.constData() + HeaderBytes),
        static_cast<unsigned long long>(encrypted.size() - HeaderBytes),
        reinterpret_cast<const unsigned char *>(aad.constData()), static_cast<unsigned long long>(aad.size()),
        reinterpret_cast<const unsigned char *>(encrypted.constData() + NonceOffset), key.bytes.data());
    if (result != 0 || size != static_cast<unsigned long long>(plain.size())) {
        wipe(plain);
        return fail(error, "Could not unlock this key. The passphrase may be incorrect, the profile may have changed, or the encrypted file may be damaged.");
    }
    *apiKey = std::move(plain);
    return true;
}

QString CredentialStore::normalizedOrigin(const QString &address)
{
    const QUrl url(address.trimmed(), QUrl::StrictMode);
    const auto scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != "https" && scheme != "http")
        || url.host().isEmpty() || !url.userInfo().isEmpty()) return {};
    QUrl origin;
    origin.setScheme(scheme);
    origin.setHost(url.host().toLower());
    const int port = url.port(-1);
    if (port != -1 && !((scheme == "https" && port == 443) || (scheme == "http" && port == 80)))
        origin.setPort(port);
    return origin.toString(QUrl::FullyEncoded);
}

void CredentialStore::wipe(QByteArray &bytes)
{
    if (!bytes.isEmpty()) sodium_memzero(bytes.data(), static_cast<size_t>(bytes.size()));
    bytes.clear();
    bytes.squeeze();
}

#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

class QThread;

struct ModelPackSpec {
    QString id;
    QString name;
    QString fileName;
    QString sha256;
    qint64 size = 0;
};

// Imports only the explicitly supported, pinned offline model packs. Archives
// are inspected and copied here; no executable or extraction utility is run.
class ModelPacks final : public QObject {
    Q_OBJECT
public:
    explicit ModelPacks(QObject *parent = nullptr);
    ~ModelPacks() override;

    static QVector<ModelPackSpec> catalog();
    static QVector<ModelPackSpec> installedModels();
    static QString resolve(const QString &id);
    static QString configDirectory();
    static QString modelsDirectory();

    bool busy() const { return m_thread != nullptr; }
    void importArchive(const QString &path);
    void cancel();

signals:
    void progress(qint64 received, qint64 total);
    void completed(QString id);
    void error(QString message);
    void cancelled();

private:
    friend class ModelPacksTest;
    struct ImportResult { QString id, error; bool cancelled = false; };
    static ImportResult importPack(const QString &archive, const QString &directory,
        const QVector<ModelPackSpec> &specs, std::atomic_bool &cancel,
        const std::function<void(qint64, qint64)> &report);
    QThread *m_thread = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
};

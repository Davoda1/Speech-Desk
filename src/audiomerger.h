#pragma once
#include <QObject>
#include <QStringList>
#include <QAudioDecoder>
#include <QSaveFile>
#include <memory>
class AudioMerger:public QObject {
    Q_OBJECT
public:
    explicit AudioMerger(QObject *parent=nullptr);
    ~AudioMerger() override;
    void start(const QStringList &files,const QString &destination);
    void cancel();
    bool busy() const {return m_active;}
signals:
    void progress(int current,int total);
    void completed(QString path);
    void error(QString message);
private:
    QAudioDecoder *m_decoder = nullptr;
    QStringList m_files;
    QString m_destination;
    std::unique_ptr<QSaveFile> m_file;
    int m_index=0;
    quint64 m_bytes=0;
    quint64 m_generation=0;
    quint64 m_segmentBytes=0;
    bool m_active=false;
    void next();
    void fail(const QString &message);
    bool drain(QAudioDecoder *decoder);
};

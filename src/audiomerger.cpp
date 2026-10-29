#include "audiomerger.h"
#include <QAudioBuffer>
#include <QDataStream>
#include <QTimer>
#include <QUrl>

AudioMerger::AudioMerger(QObject *parent):QObject(parent) {}
AudioMerger::~AudioMerger() { cancel(); }

void AudioMerger::start(const QStringList &files,const QString &destination)
{
    cancel();
    if(files.isEmpty()){emit error("There is no generated audio to export.");return;}
    m_files=files;m_destination=destination;m_file=std::make_unique<QSaveFile>(destination);
    if(!m_file->open(QIODevice::WriteOnly)){emit error(m_file->errorString());m_file.reset();return;}
    if(m_file->write(QByteArray(44,'\0'))!=44){const auto message=m_file->errorString();m_file->cancelWriting();m_file.reset();emit error(message);return;}
    m_active=true;m_index=0;m_bytes=0;next();
}

bool AudioMerger::drain(QAudioDecoder *decoder)
{
    while(m_active && decoder==m_decoder && decoder->bufferAvailable()) {
        const auto buffer=decoder->read();
        if(!buffer.isValid())break;
        if(buffer.format().sampleRate()!=48000 || buffer.format().channelCount()!=1 || buffer.format().sampleFormat()!=QAudioFormat::Int16){
            fail("This audio decoder could not provide the required WAV export format.");return false;
        }
        if(m_bytes+quint64(buffer.byteCount())>0xfffffff0ULL-44){fail("The audio exceeds the WAV file size limit.");return false;}
        if(m_file->write(buffer.constData<char>(),buffer.byteCount())!=buffer.byteCount()){
            fail(m_file->errorString());return false;
        }
        m_bytes+=buffer.byteCount();m_segmentBytes+=buffer.byteCount();
    }
    return m_active && decoder==m_decoder;
}

void AudioMerger::next()
{
    if(!m_active)return;
    if(m_index<m_files.size()) {
        const auto generation=m_generation;
        const auto section=m_index;
        // Each source owns a fresh decoder. Old completion/buffer events cannot
        // advance a later source or a newly started export.
        auto *decoder=new QAudioDecoder(this);m_decoder=decoder;m_segmentBytes=0;
        QAudioFormat format;format.setSampleRate(48000);format.setChannelCount(1);format.setSampleFormat(QAudioFormat::Int16);
        decoder->setAudioFormat(format);
        connect(decoder,&QAudioDecoder::bufferReady,this,[this,decoder,generation,section]{
            if(m_active && generation==m_generation && section==m_index && decoder==m_decoder)drain(decoder);
        });
        connect(decoder,&QAudioDecoder::finished,this,[this,decoder,generation,section]{
            if(!m_active || generation!=m_generation || section!=m_index || decoder!=m_decoder)return;
            if(!drain(decoder))return;
            if(m_segmentBytes==0){fail("A generated speech section contains no decodable audio.");return;}
            m_decoder=nullptr;decoder->disconnect(this);decoder->deleteLater();
            ++m_index;
            QTimer::singleShot(0,this,[this,generation,section]{
                if(m_active && generation==m_generation && m_index==section+1)next();
            });
        });
        connect(decoder,QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error),this,[this,decoder,generation,section](QAudioDecoder::Error){
            if(m_active && generation==m_generation && section==m_index && decoder==m_decoder)fail(decoder->errorString());
        });
        emit progress(m_index+1,m_files.size());
        if(!m_active || generation!=m_generation || decoder!=m_decoder)return;
        decoder->setSource(QUrl::fromLocalFile(m_files[m_index]));decoder->start();
        return;
    }
    if(!m_file->seek(0)){fail(m_file->errorString());return;}
    QDataStream stream(m_file.get());stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF",4);stream<<quint32(m_bytes+36);stream.writeRawData("WAVEfmt ",8);
    stream<<quint32(16)<<quint16(1)<<quint16(1)<<quint32(48000)<<quint32(96000)<<quint16(2)<<quint16(16);
    stream.writeRawData("data",4);stream<<quint32(m_bytes);
    if(stream.status()!=QDataStream::Ok){fail("Could not finish the WAV header.");return;}
    m_active=false;
    auto file=std::move(m_file);
    if(!file->commit()){emit error(file->errorString());return;}
    emit completed(m_destination);
}

void AudioMerger::fail(const QString &message){cancel();emit error(message);}
void AudioMerger::cancel()
{
    ++m_generation;m_active=false;
    if(m_decoder){auto *decoder=m_decoder;m_decoder=nullptr;decoder->disconnect(this);decoder->stop();decoder->deleteLater();}
    if(m_file){m_file->cancelWriting();m_file.reset();}
}

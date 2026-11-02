#include "audioutil.h"

#include <QAudioDeviceInfo>
#include <QAudioFormat>
#include <QAudioInput>
#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QMediaPlayer>
#include <QUrl>
#include <QFile>
#include <QDateTime>

// ============================ AudioRecorder ============================

AudioRecorder::AudioRecorder(QObject *parent) : QObject(parent)
{
}

AudioRecorder::~AudioRecorder()
{
    cancel();
}

bool AudioRecorder::isRecording() const
{
    return m_input && m_input->state() == QAudio::ActiveState;
}

int AudioRecorder::elapsedMs() const
{
    if (m_startMs <= 0) return 0;
    return static_cast<int>(QDateTime::currentMSecsSinceEpoch() - m_startMs);
}

bool AudioRecorder::start()
{
    cancel();       // 先把上一次残留清干净

    QAudioDeviceInfo device = QAudioDeviceInfo::defaultInputDevice();
    if (device.isNull())
    {
        emit signalError(tr("没有找到可用的麦克风（录音设备）"));
        return false;
    }

    QAudioFormat format;
    format.setSampleRate(m_sampleRate);
    format.setChannelCount(m_channels);
    format.setSampleSize(m_bits);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);

    // 设备不支持 16k 单声道时退到它最接近的格式，否则 start() 会静默不录
    if (!device.isFormatSupported(format))
    {
        const QAudioFormat near = device.nearestFormat(format);
        qInfo() << "[录音] 设备不支持 16k 单声道，改用" << near.sampleRate()
                << "Hz /" << near.channelCount() << "声道";
        format = near;
    }

    m_sampleRate = format.sampleRate();
    m_channels   = format.channelCount();
    m_bits       = format.sampleSize();

    m_buffer = new QBuffer(this);
    m_buffer->open(QIODevice::WriteOnly | QIODevice::Truncate);

    m_input = new QAudioInput(device, format, this);
    m_input->setNotifyInterval(200);
    m_input->start(m_buffer);

    if (m_input->error() != QAudio::NoError)
    {
        emit signalError(tr("录音启动失败（错误码 %1）").arg(static_cast<int>(m_input->error())));
        cancel();
        return false;
    }

    m_startMs = QDateTime::currentMSecsSinceEpoch();
    qInfo() << "[录音] 开始" << m_sampleRate << "Hz" << m_channels << "声道" << m_bits << "bit";
    return true;
}

QByteArray AudioRecorder::stop()
{
    if (!m_input || !m_buffer)
    {
        cancel();
        return QByteArray();
    }

    const int ms = elapsedMs();
    m_input->stop();
    m_startMs = 0;

    const QByteArray pcm = m_buffer->data();

    m_buffer->close();
    delete m_buffer;
    m_buffer = nullptr;
    delete m_input;
    m_input = nullptr;

    // 太短的录音（不到 0.3 秒）基本是误触，直接丢弃
    if (pcm.isEmpty() || ms < 300)
    {
        qInfo() << "[录音] 内容过短（" << ms << "ms），已丢弃";
        return QByteArray();
    }

    const QByteArray wav = pcmToWav(pcm);
    qInfo() << "[录音] 结束 时长" << ms << "ms 体积" << wav.size() << "字节";
    return wav;
}

void AudioRecorder::cancel()
{
    if (m_input)
    {
        m_input->stop();
        delete m_input;
        m_input = nullptr;
    }
    if (m_buffer)
    {
        m_buffer->close();
        delete m_buffer;
        m_buffer = nullptr;
    }
    m_startMs = 0;
}

/**
 * @brief AudioRecorder::pcmToWav
 * 给裸 PCM 数据补一个 44 字节标准 WAV 头，QMediaPlayer 才能直接播放
 */
QByteArray AudioRecorder::pcmToWav(const QByteArray &pcm) const
{
    if (pcm.isEmpty()) return QByteArray();

    const int channels  = qMax(1, m_channels);
    const int bits      = qMax(8, m_bits);
    const int sampleRate = qMax(8000, m_sampleRate);
    const int blockAlign = channels * bits / 8;
    const quint32 byteRate = static_cast<quint32>(sampleRate) * blockAlign;
    const quint32 dataSize = static_cast<quint32>(pcm.size());

    QByteArray wav;
    wav.reserve(44 + pcm.size());

    auto put32 = [&wav](quint32 v) {
        wav.append(static_cast<char>(v & 0xFF));
        wav.append(static_cast<char>((v >> 8) & 0xFF));
        wav.append(static_cast<char>((v >> 16) & 0xFF));
        wav.append(static_cast<char>((v >> 24) & 0xFF));
    };
    auto put16 = [&wav](quint16 v) {
        wav.append(static_cast<char>(v & 0xFF));
        wav.append(static_cast<char>((v >> 8) & 0xFF));
    };

    wav.append("RIFF", 4);
    put32(36 + dataSize);          // ChunkSize
    wav.append("WAVE", 4);

    wav.append("fmt ", 4);
    put32(16);                     // Subchunk1Size（PCM 固定 16）
    put16(1);                      // AudioFormat = PCM
    put16(static_cast<quint16>(channels));
    put32(static_cast<quint32>(sampleRate));
    put32(byteRate);
    put16(static_cast<quint16>(blockAlign));
    put16(static_cast<quint16>(bits));

    wav.append("data", 4);
    put32(dataSize);
    wav.append(pcm);

    return wav;
}

/**
 * @brief AudioRecorder::wavDurationMs
 * 从 WAV 头部读采样率 / 声道 / 位深，算出时长。
 * 老服务端或对端没带 duration 字段时用它兜底。
 */
int AudioRecorder::wavDurationMs(const QByteArray &wav)
{
    if (wav.size() < 44) return 0;
    if (!wav.startsWith("RIFF") || wav.mid(8, 4) != "WAVE") return 0;

    auto get32 = [&wav](int pos) -> quint32 {
        return static_cast<quint32>(static_cast<quint8>(wav.at(pos)))
                | (static_cast<quint32>(static_cast<quint8>(wav.at(pos + 1))) << 8)
                | (static_cast<quint32>(static_cast<quint8>(wav.at(pos + 2))) << 16)
                | (static_cast<quint32>(static_cast<quint8>(wav.at(pos + 3))) << 24);
    };
    auto get16 = [&wav](int pos) -> quint16 {
        return static_cast<quint16>(static_cast<quint8>(wav.at(pos)))
                | (static_cast<quint16>(static_cast<quint8>(wav.at(pos + 1))) << 8);
    };

    int pos = 12;
    int sampleRate = 0, channels = 0, bits = 0;
    quint32 dataSize = 0;

    while (pos + 8 <= wav.size())
    {
        const QByteArray chunkId = wav.mid(pos, 4);
        const quint32 chunkSize = get32(pos + 4);
        const int body = pos + 8;

        if (chunkId == "fmt " && body + 16 <= wav.size())
        {
            channels   = get16(body + 2);
            sampleRate = static_cast<int>(get32(body + 4));
            bits       = get16(body + 14);
        }
        else if (chunkId == "data")
        {
            // 录音过程中头部长度可能还没回填，这里以实际剩余字节为准
            dataSize = static_cast<quint32>(qMin<qint64>(chunkSize, wav.size() - body));
        }

        pos = body + static_cast<int>(chunkSize) + (chunkSize & 1);
    }

    if (sampleRate <= 0 || channels <= 0 || bits <= 0 || dataSize == 0) return 0;

    const double seconds = static_cast<double>(dataSize)
            / (sampleRate * channels * (bits / 8.0));
    return static_cast<int>(seconds * 1000);
}

// ============================= AudioPlayer =============================

AudioPlayer *AudioPlayer::instance()
{
    static AudioPlayer *self = new AudioPlayer();
    return self;
}

AudioPlayer::AudioPlayer(QObject *parent) : QObject(parent)
{
    m_player = new QMediaPlayer(this);

    connect(m_player, &QMediaPlayer::stateChanged, this,
            [this](QMediaPlayer::State state)
    {
        if (state == QMediaPlayer::StoppedState && !m_path.isEmpty())
        {
            const QString path = m_path;
            m_path.clear();
            emit signalFinished(path);
        }
    });

    connect(m_player, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error), this,
            [this](QMediaPlayer::Error)
    {
        const QString err = m_player->errorString();
        qWarning() << "[语音] 播放失败：" << err;
        appendAudioLog(QString("[AudioPlayer] error=%1 msg=%2").arg(m_player->error()).arg(err));
        m_path.clear();
        emit signalError(err);
    });
    connect(m_player, &QMediaPlayer::stateChanged, this, [this](QMediaPlayer::State s) {
        appendAudioLog(QString("[AudioPlayer] stateChanged=%1").arg(s));
    });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus s) {
        appendAudioLog(QString("[AudioPlayer] mediaStatusChanged=%1").arg(s));
    });
}

AudioPlayer::~AudioPlayer()
{
    delete m_player;
}

bool AudioPlayer::isPlaying() const
{
    return m_player && m_player->state() == QMediaPlayer::PlayingState;
}

void AudioPlayer::play(const QString &filePath)
{
    QString log = QString("[AudioPlayer] play 调用 path=%1 exists=%2")
            .arg(filePath).arg(QFile::exists(filePath));
    appendAudioLog(log);

    if (filePath.isEmpty()) return;

    if (!QFile::exists(filePath))
    {
        appendAudioLog("[AudioPlayer] 文件不存在，放弃播放");
        emit signalError(tr("语音文件不存在：%1").arg(filePath));
        return;
    }

    // 再点同一条就是停止（QQ 的做法）
    if (isPlaying() && m_path == filePath)
    {
        stop();
        return;
    }

    m_player->stop();
    m_path = filePath;
    m_player->setMedia(QUrl::fromLocalFile(filePath));
    m_player->setVolume(100);
    m_player->play();
    appendAudioLog(QString("[AudioPlayer] setMedia+play 完成 state=%1 status=%2")
            .arg(m_player->state()).arg(m_player->mediaStatus()));

    emit signalStarted(filePath);
}

void AudioPlayer::appendAudioLog(const QString &msg)
{
    QFile f("D:/qt_audio_play.log");
    f.open(QIODevice::Append | QIODevice::Text);
    f.write((QDateTime::currentDateTime().toString("hh:mm:ss.zzz ") + msg + "\n").toUtf8());
    f.close();
}

void AudioPlayer::stop()
{
    if (!m_player) return;
    m_player->stop();
    if (!m_path.isEmpty())
    {
        const QString path = m_path;
        m_path.clear();
        emit signalFinished(path);
    }
}

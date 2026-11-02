#ifndef AUDIOUTIL_H
#define AUDIOUTIL_H

#include <QObject>
#include <QByteArray>
#include <QString>

class QAudioInput;
class QBuffer;
class QMediaPlayer;

/**
 * 语音录制：把麦克风 PCM 数据录进内存，停止时打包成 WAV。
 * 采样率固定 16kHz / 16bit / 单声道 —— 语音够用，10 秒约 320KB，
 * 再走 base64 内嵌进消息，由端到端加密统一保护。
 */
class AudioRecorder : public QObject
{
    Q_OBJECT
public:
    explicit AudioRecorder(QObject *parent = nullptr);
    ~AudioRecorder();

    bool start();                   // 开始录音；失败时发出 signalError
    QByteArray stop();              // 停止并返回 WAV 数据（没录到内容返回空）
    void cancel();                  // 丢弃本次录音
    bool isRecording() const;

    int  elapsedMs() const;         // 本次已录时长（毫秒）

    static int wavDurationMs(const QByteArray &wav);   // 解析 WAV 头算时长

signals:
    void signalError(const QString &msg);

private:
    QByteArray pcmToWav(const QByteArray &pcm) const;

    QAudioInput *m_input  = nullptr;
    QBuffer     *m_buffer = nullptr;
    qint64       m_startMs = 0;

    int m_sampleRate = 16000;
    int m_channels   = 1;
    int m_bits       = 16;
};

/**
 * 语音播放：全局只有一个播放器，同一时刻只放一条语音。
 */
class AudioPlayer : public QObject
{
    Q_OBJECT
public:
    static AudioPlayer *instance();

    void play(const QString &filePath);
    void stop();
    bool isPlaying() const;
    QString currentPath() const { return m_path; }

signals:
    void signalStarted(const QString &path);
    void signalFinished(const QString &path);
    void signalError(const QString &msg);

private:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer();

    QMediaPlayer *m_player = nullptr;
    QString       m_path;

    void appendAudioLog(const QString &msg);
};

#endif // AUDIOUTIL_H

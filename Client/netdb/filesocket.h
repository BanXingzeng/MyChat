#ifndef FILESOCKET_H
#define FILESOCKET_H

#include <QObject>
#include <QTcpSocket>
#include <QFile>
#include <QTimer>
#include <QDataStream>
#include <QJsonObject>

#include "comapi/cryptoutil.h"

class FileSocket : public QObject
{
    Q_OBJECT
public:
    explicit FileSocket(QObject *parent = nullptr);
    ~FileSocket();

    // 连接文件服务器
    void connectToFileServer(const QString &host, quint16 port);
    // 设置发送文件路径
    void setFilePath(const QString &filePath);
    // 设置分片加载大小（单次读取/发送的字节数）
    void setLoadSize(quint64 loadSize);
    // 设置用户ID
    void setUserId(const QString &userId);
    // 设置文件保存路径（接收文件时）
    void setSavePath(const QString &saveDir);
    // 启动发送文件
    void startSendFile();
    // 停止发送/接收文件
    void stopSendFile();
    // 请求下载文件（向服务器发送下载指令）
    void requestFile(const QString &remoteFileName);

    // ===== 端到端加密（文件内容）=====
    // 发送端：一次性会话密钥 + 各接收者公钥包裹后的元数据（JSON 字节）
    void setEncryptSession(const QByteArray &key, const QByteArray &iv, const QByteArray &metaJson);
    // 接收端：本机身份，用于从文件头解出属于自己的那份会话密钥
    void setDecryptIdentity(int userId, const QString &privPem);
    // 本端是否已具备加解密条件
    bool hasEncryptSession() const;

signals:
    // 文件服务器连接成功
    void fileServerConnected();
    // 进度更新：已发送/接收字节数、总字节数
    void progressUpdated(quint64 processedBytes, quint64 totalBytes);
    // 发送完成
    void sendFinished();
    // 接收完成，参数：保存的文件路径
    void recvFinished(const QString &savePath);
    // 发送/接收失败，参数：错误信息
    void sendFailed(const QString &errorMsg);

private slots:
    // Socket连接成功槽函数
    void onSocketConnected();
    // Socket错误处理
    void onSocketError(QAbstractSocket::SocketError err);
    // Socket写入字节数回调（发送文件时）
    void onBytesWritten(qint64 numBytes);
    // Socket可读（接收文件时）
    void onReadyRead();

private:
    // 重置发送状态（清空计数、缓冲区等）
    void resetSendState();
    // 用文件头里的 meta 初始化接收端解密流
    bool initRecvCrypto(const QByteArray &metaBytes);

private:
    QTcpSocket *m_tcpSocket = nullptr;      // TCP Socket
    QFile *m_fileToSend = nullptr;          // 待发送的文件对象
    QFile *m_fileToRecv = nullptr;          // 待接收的文件对象
    QTimer *m_Time = nullptr;               // 进度更新计时器（节流用）

    // 发送文件相关变量
    QString m_filePath;                     // 待发送文件路径
    quint64 m_loadSize = 4 * 1024;          // 分片大小（默认4KB）
    quint64 m_totalBytes = 0;               // 发送文件总字节数（含包头）
    quint64 m_bytesWritten = 0;             // 已发送字节数
    quint64 m_bytesToWrite = 0;             // 待发送字节数
    QByteArray m_outBlock;                  // 发送缓冲区

    // 接收文件相关变量
    QString m_saveDir;                      // 接收文件保存目录
    qint64 m_recvTotalBytes = 0;            // 接收文件总字节数
    qint64 m_recvProcessedBytes = 0;        // 已接收/处理字节数
    QByteArray m_recvBuffer;                // 接收缓冲区

    // 端到端加密（文件内容）
    QByteArray m_sendKey;                   // 发送会话密钥（32 字节）
    QByteArray m_sendIv;                    // 发送会话 IV（16 字节）
    QByteArray m_sendMeta;                  // 发送端加密头（JSON）
    int        m_decryptUid = 0;            // 接收端本机用户 ID
    QString    m_decryptPriv;               // 接收端本机私钥 PEM
    CryptoStream m_stream;                  // 收发共用的 AES-256-CTR 流

    // 状态标记
    bool m_isSending = false;               // 是否正在发送文件
    bool m_isReceiving = false;             // 是否正在接收文件
    bool m_isStopped = false;               // 是否手动停止传输
    QString m_userId;                       // 用户ID
};

#endif // FILESOCKET_H

#ifndef MYSOCKET_H
#define MYSOCKET_H

#include <QFile>
#include <QTcpSocket>
#include <QTimer>

class MySocket : public QObject
{
    Q_OBJECT
public:
    explicit MySocket(QObject *parent = nullptr);
    ~MySocket();
    void CheckConnected();
    void ColseConnected();          // 主动断开：不会再触发自动重连

    // 断线自动重连：默认开启；退出登录/主动离线时由 ColseConnected() 关掉
    void setAutoReconnect(bool enable);
    bool autoReconnect() const { return m_bAutoReconnect; }
    bool isReconnecting() const { return m_reconnectTimer && m_reconnectTimer->isActive(); }

    void connectToHost(const QString &ip, quint16 port);
    void connectToHost(const QHostAddress &host, quint16 port); // 与服务器建立Tcp连接

    void ParseLogin(const QJsonValue &dataVal);
    void sendMsgType(const quint8 &nType,const QJsonValue &dataVal);

    QJsonValue GetUserId();
    bool isConnected() const;      // 当前是否已连接到服务器
    QAbstractSocket::SocketState state() const;   // 当前连接状态（未连接/连接中/已连接）

signals:
    void signalStatus(const quint8 &status,const QJsonValue &dataVal);
    void signalConnectSuccess();
    void signalConnectFailed(const QString &errorString);
    // 链路中断，已排入第 attempt 次重连（delayMs 毫秒后发起）
    void signalConnectionLost(int attempt, int delayMs);
    // 断线后重新连上（首次连接不会发这个信号），上层据此重新登录
    void signalReconnected();

public slots:
    void sendMessage(const quint8 &type, const QJsonValue &dataVal);

private slots:
    void sltReadyRead();
    void sltConnect();
    void sltDisconnect();
    void sltError(QAbstractSocket::SocketError error);
    void sltReconnectTimeout();     // 退避到点，发起一次重连


private:
    void scheduleReconnect();       // 安排下一次重连（指数退避）
    void stopReconnect();

    QTcpSocket *m_tcpSocket;
    QByteArray m_recvBuffer;    //缓存未处理的字节
    int m_nId;

    QTimer *m_reconnectTimer = nullptr;
    bool    m_bAutoReconnect = true;    // 是否允许自动重连
    bool    m_bManualClose   = false;   // 本次断开是否由本端主动发起
    bool    m_bConnectedOnce = false;   // 是否已经连上过（用于区分首连/重连）
    int     m_nRetryCount    = 0;       // 连续失败次数，决定退避时长
};

#endif // MYSOCKET_H

#include "mysocket.h"
#include <QJsonObject>
#include <qjsondocument.h>
#include <QHostAddress>
#include <QTimer>
#include "comapi/unit.h"
#include <comapi/myapp.h>

MySocket::MySocket(QObject *parent)
{
    m_tcpSocket = new QTcpSocket(this);
    m_nId = MyApp::m_nId;
    connect(m_tcpSocket,&QTcpSocket::connected,this,&MySocket::sltConnect);
    connect(m_tcpSocket,&QTcpSocket::readyRead,this,&MySocket::sltReadyRead);
    connect(m_tcpSocket,&QTcpSocket::disconnected,this,&MySocket::sltDisconnect);
    connect(m_tcpSocket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
            this, &MySocket::sltError);

    // 断线重连定时器：单次触发，每次由 scheduleReconnect() 按退避时长重新 arm
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &MySocket::sltReconnectTimeout);
}

MySocket::~MySocket()
{

}

bool MySocket::isConnected() const
{
    return m_tcpSocket->state() == QAbstractSocket::ConnectedState;
}

QAbstractSocket::SocketState MySocket::state() const
{
    return m_tcpSocket->state();
}

void MySocket::CheckConnected()
{
    if (m_tcpSocket->state() != QTcpSocket::ConnectedState)
    {
        m_tcpSocket->connectToHost(MyApp::m_strHostAddr, MyApp::m_nMsgPort);
    }
}

void MySocket::ColseConnected()
{
    // 主动断开（托盘"离线"、退出程序）不参与自动重连，否则会一直重连回来
    m_bManualClose = true;
    stopReconnect();
    m_nRetryCount = 0;
    if (m_tcpSocket->isOpen()) m_tcpSocket->abort();
}

void MySocket::setAutoReconnect(bool enable)
{
    m_bAutoReconnect = enable;
    if (!enable) stopReconnect();
}

/**
 * @brief MySocket::scheduleReconnect
 * 指数退避：1s → 2s → 4s → 8s → 16s → 30s，之后固定 30s。
 * 这样服务器刚重启时不会几千个客户端同时砸上来，也能在恢复后尽快重连。
 */
void MySocket::scheduleReconnect()
{
    if (!m_bAutoReconnect || m_bManualClose) return;
    if (isReconnecting()) return;      // 已经排好队了，别叠加

    const int delay = qMin(30000, 1000 * (1 << qMin(m_nRetryCount, 5)));
    ++m_nRetryCount;

    qInfo() << "[重连] 第" << m_nRetryCount << "次重连将在" << delay << "ms 后发起";
    emit signalConnectionLost(m_nRetryCount, delay);

    m_reconnectTimer->start(delay);
}

void MySocket::stopReconnect()
{
    if (m_reconnectTimer) m_reconnectTimer->stop();
}

void MySocket::sltReconnectTimeout()
{
    if (!m_bAutoReconnect || m_bManualClose) return;

    const QString ip = MyApp::m_strHostAddr;
    const quint16 port = MyApp::m_nMsgPort;
    if (ip.isEmpty() || 0 == port) return;

    if (m_tcpSocket->state() != QAbstractSocket::UnconnectedState) {
        m_tcpSocket->abort();       // 半开连接先强断，否则 connectToHost 会被忽略
    }
    m_recvBuffer.clear();
    qInfo() << "[重连] 正在连接" << ip << port;
    m_tcpSocket->connectToHost(ip, port);
}


void MySocket::connectToHost(const QString &ip, quint16 port)
{
    // 先断开已有连接（无论是已连接还是正在连接），避免重复连接
    if (m_tcpSocket->state() != QAbstractSocket::UnconnectedState) {
        m_tcpSocket->abort();
    }
    // 换服务器后旧的半包数据必须丢掉，否则解析会错乱
    m_recvBuffer.clear();

    // 手动指定地址重连：重新允许自动重连
    m_bManualClose = false;
    m_nRetryCount = 0;
    stopReconnect();

    // 转发给内部的 m_tcpSocket，建立连接
    m_tcpSocket->connectToHost(ip, port);

    qDebug() << "尝试连接服务器:" << ip << port;
}

void MySocket::connectToHost(const QHostAddress &host, quint16 port)
{
    if (m_tcpSocket->state() != QAbstractSocket::UnconnectedState) {
        m_tcpSocket->abort();
    }
    m_recvBuffer.clear();

    m_bManualClose = false;
    m_nRetryCount = 0;
    stopReconnect();

    m_tcpSocket->connectToHost(host, port);

    qDebug() << "尝试连接服务器:" << host.toString() << port;
}

void MySocket::sltReadyRead()
{
    QByteArray newData = m_tcpSocket->readAll();
    m_recvBuffer.append(newData);
    // 图片消息是 base64 内嵌的，整包可能几百 KB，日志里只留摘要
    if (newData.size() <= 512)
        qDebug() << "客户端收到消息：" << newData;
    else
        qDebug() << "客户端收到消息：大包" << newData.size() << "字节";
    while (true) {
        // 先检查是否够4字节长度头
        if (m_recvBuffer.size() < 4) {
            break; // 数据不足，等待下次
        }

        quint32 dataLen = 0;
        dataLen |= (static_cast<quint8>(m_recvBuffer[0]) << 24);
        dataLen |= (static_cast<quint8>(m_recvBuffer[1]) << 16);
        dataLen |= (static_cast<quint8>(m_recvBuffer[2]) << 8);
        dataLen |= static_cast<quint8>(m_recvBuffer[3]);

        if (m_recvBuffer.size() < 4 + dataLen) {
            break; // 数据未收全，等待下次
        }
        QByteArray jsonData = m_recvBuffer.mid(4, dataLen);
        m_recvBuffer.remove(0, 4 + dataLen);

        QJsonDocument doc = QJsonDocument::fromJson(jsonData);
        if (doc.isObject()) {
            QJsonObject jsonObj = doc.object();
            quint8 nType = jsonObj["type"].toInt();
            QString from = jsonObj["from"].toString();
            QJsonValue dataVal = jsonObj["data"];

            sendMsgType(nType,dataVal);
        }
    }
}

void MySocket::sltConnect()
{
    m_recvBuffer.clear();
    m_bManualClose = false;
    m_nRetryCount = 0;
    stopReconnect();

    qDebug() << "已连接服务器:" << m_tcpSocket->peerAddress().toString()
             << m_tcpSocket->peerPort();

    const bool isReconnect = m_bConnectedOnce;   // 之前连上过 = 这次是恢复
    m_bConnectedOnce = true;

    emit signalConnectSuccess();
    if (isReconnect) {
        qInfo() << "[重连] 链路已恢复";
        emit signalReconnected();
    }
}

void MySocket::sltDisconnect()
{
    qDebug() << "客户端断开连接";
    if (m_bManualClose || !m_bAutoReconnect) return;
    scheduleReconnect();
}

void MySocket::sltError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error)
    // 远端正常关闭不算连接失败，避免重复提示
    if (m_tcpSocket->error() == QAbstractSocket::RemoteHostClosedError) {
        return;
    }
    qWarning() << "连接服务器失败:" << m_tcpSocket->errorString();
    emit signalConnectFailed(m_tcpSocket->errorString());

    // 服务器没起、网络不通、DNS 失败……都走同一条退避重连
    if (m_bAutoReconnect && !m_bManualClose) {
        scheduleReconnect();
    }
}


void MySocket::sendMessage(const quint8 &type, const QJsonValue &dataVal)
{
    // 连接服务器
    if (!m_tcpSocket->isOpen()) {
        m_tcpSocket->connectToHost(MyApp::m_strHostAddr, MyApp::m_nMsgPort);
        m_tcpSocket->waitForConnected(1000);
    }
    // 超时1s后还是连接不上，直接返回
    if (!m_tcpSocket->isOpen()) return;
    QJsonObject json;
    json.insert("type",type);
    json.insert("from", MyApp::m_nId);
    json.insert("data",dataVal);

    QJsonDocument document;
    document.setObject(json);
    QByteArray jsonData = document.toJson(QJsonDocument::Compact);
    // 大包（图片）只打印摘要，避免日志被 base64 淹没
    if (jsonData.size() <= 512)
        qDebug() << "客户端发送消息：" << jsonData;
    else
        qDebug() << "客户端发送消息：大包" << jsonData.size() << "字节 type" << type;

    //防止粘包
    quint32 dataLen = static_cast<quint32>(jsonData.size());
    QByteArray lenBytes;
    lenBytes.resize(4);
    lenBytes[0] = (dataLen >> 24) & 0xFF;
    lenBytes[1] = (dataLen >> 16) & 0xFF;
    lenBytes[2] = (dataLen >> 8) & 0xFF;
    lenBytes[3] = dataLen & 0xFF;
    QByteArray sendData = lenBytes + jsonData;

    m_tcpSocket->write(sendData);
}

void MySocket::sendMsgType(const quint8 &nType, const QJsonValue &dataVal)
{
    switch (nType) {
    case LoginSuccess:
    {
        emit signalStatus(LoginSuccess,dataVal);
        break;
    }
    case LoginRepeat:
    {
        emit signalStatus(LoginRepeat,dataVal);
        break;
    }
    case LoginPasswdError:
    {
        emit signalStatus(LoginPasswdError,dataVal);
        break;
    }
    case RegisterOk:
    {
        emit signalStatus(RegisterOk,dataVal);
        break;
    }
    case RegisterFailed:
    {
        emit signalStatus(RegisterFailed,dataVal);
        break;
    }
    case AddFriendFailed_NoneUser:
    {
        emit signalStatus(AddFriendFailed_NoneUser,dataVal);
        break;
    }
    case AddFriendRequist:
    {
        emit signalStatus(AddFriendRequist, dataVal);
        break;
    }
    case AddFriendReply:
    {
        emit signalStatus(AddFriendReply,dataVal);
        break;
    }
    case GetMyFriends:
    {
        emit signalStatus(GetMyFriends, dataVal);
        break;
    }
    case RefreshFriends:
    {
        emit signalStatus(RefreshFriends, dataVal);
        break;
    }

    case SendMsg:
    {
        emit signalStatus(SendMsg, dataVal);
        break;
    }
    case SendFace:
    {
        emit signalStatus(SendFace, dataVal);
        break;
    }
    case SendFile:
    {
        emit signalStatus(SendFile, dataVal);
        break;
    }
    case DeleteFriend:
    {
        emit signalStatus(DeleteFriend, dataVal);
        break;
    }
    case CreateGroup:
    {
        emit signalStatus(CreateGroup, dataVal);
        break;
    }
    case GetMyGroups:
    {
        emit signalStatus(GetMyGroups, dataVal);
        break;
    }
    case RefreshGroups:
    {
        emit signalStatus(RefreshGroups, dataVal);
        break;
    }
    case AddGroupRequist:
    {
        emit signalStatus(AddGroupRequist, dataVal);
        break;
    }
    case AddGroupReply:
    {
        emit signalStatus(AddGroupReply, dataVal);
        break;
    }
    case AddGroupAccept:
    {
        emit signalStatus(AddGroupAccept, dataVal);
        break;
    }
    case SendGroupMsg:
    {
        emit signalStatus(SendGroupMsg, dataVal);
        break;
    }
    case QuitGroupOk:
    {
        emit signalStatus(QuitGroupOk, dataVal);
        break;
    }
    case DissolveGroupOk:
    {
        emit signalStatus(DissolveGroupOk, dataVal);
        break;
    }
    case GroupDissolved:
    {
        emit signalStatus(GroupDissolved, dataVal);
        break;
    }
    case GroupOperateFailed:
    {
        emit signalStatus(GroupOperateFailed, dataVal);
        break;
    }
    case UpdateHeadOk:
    {
        emit signalStatus(UpdateHeadOk, dataVal);
        break;
    }
    case HeadChanged:
    {
        emit signalStatus(HeadChanged, dataVal);
        break;
    }
    case HeadData:
    {
        emit signalStatus(HeadData, dataVal);
        break;
    }
    case HeadOperateFailed:
    {
        emit signalStatus(HeadOperateFailed, dataVal);
        break;
    }
    case ChangePasswdOk:
    {
        emit signalStatus(ChangePasswdOk, dataVal);
        break;
    }
    case ChangePasswdFailed:
    {
        emit signalStatus(ChangePasswdFailed, dataVal);
        break;
    }
    // ===== 端到端加密：公钥交换 =====
    case UploadKeysOk:
    {
        emit signalStatus(UploadKeysOk, dataVal);
        break;
    }
    case PubKeyData:
    {
        emit signalStatus(PubKeyData, dataVal);
        break;
    }
    case KeyOperateFailed:
    {
        emit signalStatus(KeyOperateFailed, dataVal);
        break;
    }
    case HeartBeatAck:
    {
        // 心跳应答只用于确认链路通畅，界面无需响应
        break;
    }
    case RecallMsg:
    {
        emit signalStatus(RecallMsg, dataVal);
        break;
    }
    // ===== 群成员管理 =====
    case GroupMembers:
    {
        emit signalStatus(GroupMembers, dataVal);
        break;
    }
    case GroupMemberChanged:
    {
        emit signalStatus(GroupMemberChanged, dataVal);
        break;
    }
    case GroupKicked:
    {
        emit signalStatus(GroupKicked, dataVal);
        break;
    }
    // ===== 发送回执 =====
    case SendAck:
    {
        emit signalStatus(SendAck, dataVal);
        break;
    }
    }
}


QJsonValue MySocket::GetUserId()
{
    return MyApp::m_nId;
}

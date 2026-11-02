#include "filesocket.h"
#include "myserver.h"
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonValue>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include "databasemag.h"
#include "comapi/unit.h"
#include <comapi/myapp.h>

MyServer::MyServer(QObject *parent)
{
    m_tcpServer = new QTcpServer(this);

    connect(m_tcpServer, SIGNAL(newConnection()), this, SLOT(SltNewConnection()));
}

MyServer::~MyServer()
{
    if (m_tcpServer->isListening())
        m_tcpServer->close();
}

bool MyServer::StartListen(int port)
{
    if (m_tcpServer->isListening()) {
        m_tcpServer->close();
    }
    if (!m_tcpServer->listen(QHostAddress::Any, static_cast<quint16>(port))) {
        qWarning() << "[服务器] 监听端口失败：" << port << m_tcpServer->errorString();
        return false;
    }
    qDebug() << "[服务器] 开启监听" << port;
    return true;
}

void MyServer::CloseListen()
{
    if (!m_tcpServer->isListening()) {
        return;
    }
    m_tcpServer->close();
    qDebug() << "[服务器] 已停止监听";
}

bool MyServer::isListening() const
{
    return m_tcpServer && m_tcpServer->isListening();
}

int MyServer::listenPort() const
{
    return (m_tcpServer && m_tcpServer->isListening()) ? m_tcpServer->serverPort() : -1;
}

// 消息服务器
TcpMsgServer *TcpMsgServer::s_instance = nullptr;

TcpMsgServer::TcpMsgServer(QObject *parent)
{
    s_instance = this;
    m_groupMembersCache = DataBaseMag::getInstance()->initGroupMembersCache();
    qDebug() << m_groupMembersCache;

    // 心跳扫描：客户端每 15 秒发一次心跳，这里超过 60 秒没动静就判定为掉线。
    // 进程被强杀、网线被拔这类情况，TCP 层可能很久都感知不到，只能靠心跳兜底。
    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(20000);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &TcpMsgServer::sweepIdleClients);
    m_heartbeatTimer->start();
}

TcpMsgServer *TcpMsgServer::instance()
{
    return s_instance;
}

bool TcpMsgServer::isUserOnline(int userId) const
{
    if (userId <= 0) return false;
    return m_clientHash.contains(QString::number(userId));
}

/**
 * @brief 清理长时间没有任何数据（含心跳）的连接
 * 这一步保证"用户已经不存在了"时，服务器上的在线状态会自己恢复，而不是一直挂着。
 */
void TcpMsgServer::sweepIdleClients()
{
    if (m_clientHash.isEmpty()) return;

    const QDateTime now = QDateTime::currentDateTime();
    QList<ClientSocket *> stale;
    for (auto it = m_clientHash.constBegin(); it != m_clientHash.constEnd(); ++it) {
        ClientSocket *client = it.value();
        if (!client) continue;
        if (client->lastActiveTime().secsTo(now) >= 60)
            stale.append(client);
    }

    for (ClientSocket *client : stale) {
        const int userId = client->GetUserId();
        const QString userName = DataBaseMag::getInstance()->getUsernameFromId(QString::number(userId));
        qInfo() << "[心跳] 连接超时，判定下线：" << userId << userName;

        // 先从连接表摘掉并置离线，再断开；断开回调里会因为 hash 已不含它而跳过重复处理
        if (m_clientHash.value(QString::number(userId)) == client)
            m_clientHash.remove(QString::number(userId));
        if (userId > 0)
            DataBaseMag::getInstance()->UpdateUserStatus(userId, OffLine);

        emit signalServerLog(QString("用户 %1 心跳超时，已判定离线（在线 %2 人）")
                             .arg(userName.isEmpty() ? QString::number(userId) : userName)
                             .arg(m_clientHash.size()));
        client->kick();
    }

    if (!stale.isEmpty())
        emit signalClientChanged(m_clientHash.size(), m_clients.size());
}

TcpMsgServer::~TcpMsgServer()
{
    if (s_instance == this) s_instance = nullptr;
}

int TcpMsgServer::onlineCount() const
{
    return m_clientHash.size();
}

int TcpMsgServer::socketCount() const
{
    return m_clients.size();
}

/**
 * @brief TcpMsgServer::reloadGroupCache
 * 数据库被还原 / 群成员表被外部修改后，刷新内存中的群成员缓存
 */
void TcpMsgServer::reloadGroupCache()
{
    m_groupMembersCache = DataBaseMag::getInstance()->initGroupMembersCache();
    qInfo() << "[群缓存] 已刷新，群数量：" << m_groupMembersCache.size();
}


void TcpMsgServer::insertMessageQueue(const int &send, const int &getId, const QJsonValue &jsonVal, const quint8 &type)
{
    if (!jsonVal.isObject())
    {
        qDebug() << "server插入消息队列逻辑有误";
        return;
    }
    QJsonObject jsonObj = jsonVal.toObject();
    DataBaseMag::getInstance()->insertMessageQueue(send, getId, type, jsonObj);
}

void TcpMsgServer::sendUserMessageQueue(const QString &userId) // 上线的Id
{
    if (userId.isEmpty())
    {
        qDebug() << "转发上线消息：用户Id不能为空";
        return;
    }
    QList<QVariantMap> msgList = DataBaseMag::getInstance()->getUserMessageQueue(userId.toInt());
    foreach (QVariantMap msg, msgList)
    {
        quint8 type = static_cast<quint8>(msg["message_type"].toInt());
        int id = msg["request_userId"].toInt();
        int targetId = msg["accept_userId"].toInt();
        QString friendName = DataBaseMag::getInstance()->getUsernameFromId(QString::number(targetId));
        QJsonObject jsonObj = msg["data"].toJsonObject();
        if(type != SendGroupMsg && type != SendMsg &&type != SendFace &&type != SendFile &&type != SendPicture) {
            jsonObj.insert("type", type);
            jsonObj.insert("name", friendName);
        }
        qDebug() << "myServer:80 转发离线队列消息给" << friendName;
        QJsonValue sendVal = jsonObj;
        SltPrivateMsgToClient(id, targetId, type, sendVal);
    }
}

void TcpMsgServer::SltAddGroupMembers(const int &groupId, const int &userId)
{
    if(groupId < 0 || userId < 0) return;
    m_groupMembersCache[groupId].insert(userId);
}

void TcpMsgServer::SltRemoveGroupMembers(const int &groupId, const int &userId)
{
    if(groupId < 0 || userId < 0) return;
    m_groupMembersCache[groupId].remove(userId);
}

void TcpMsgServer::SltTransFileToClient(const int &userId, const QJsonValue &jsonVal)
{
}

void TcpMsgServer::SltNewConnection()
{
    QTcpSocket *socket = m_tcpServer->nextPendingConnection();
    ClientSocket *client = new ClientSocket(this, socket);
    m_clients.append(client); // 加入客户端列表管理

    // 信号槽绑定
    connect(client, &ClientSocket::signalConnected, this, &TcpMsgServer::SltConnected);
    connect(client, &ClientSocket::signalDisConnected, this, &TcpMsgServer::SltDisConnected);
    connect(client, &ClientSocket::signalLoginSuccess, this, &TcpMsgServer::SltLoginSuccess);
    connect(client, &ClientSocket::signalPrivateMsgToClient, this, &TcpMsgServer::SltPrivateMsgToClient);
    connect(client, &ClientSocket::signalGroupMsgToClient, this, &TcpMsgServer::SltGroupMsgToClient);
    connect(client, &ClientSocket::signalAddGroupMembers, this, &TcpMsgServer::SltAddGroupMembers);
    connect(client, &ClientSocket::signalRemoveGroupMembers, this, &TcpMsgServer::SltRemoveGroupMembers);
    connect(client, &ClientSocket::signalHeadChanged, this, &TcpMsgServer::SltHeadChangedToClient);

    QString addr = "未知地址";
    if (socket) {
        addr = QString("%1:%2").arg(socket->peerAddress().toString()).arg(socket->peerPort());
    }
    qDebug() << "[消息服务器] 新客户端连接！socketDescriptor：" << addr;
    emit signalServerLog(QString("新的客户端连接：%1").arg(addr));
    emit signalClientChanged(m_clientHash.size(), m_clients.size());
}

void TcpMsgServer::SltConnected()
{
}

void TcpMsgServer::SltDisConnected()
{
    ClientSocket *client = (ClientSocket *)this->sender();
    const int userId = client ? client->GetUserId() : -1;
    const QString userName = (userId > 0)
            ? DataBaseMag::getInstance()->getUsernameFromId(QString::number(userId))
            : QString();

    m_clients.removeOne(client);

    // 只有当连接表里记录的正是这条连接时才算真正下线。
    // 同一账号被顶号后，旧连接迟到的断开通知不能把新连接从在线表里删掉。
    bool reallyOffline = false;
    if (userId > 0 && m_clientHash.value(QString::number(userId)) == client) {
        m_clientHash.remove(QString::number(userId));
        DataBaseMag::getInstance()->UpdateUserStatus(userId, OffLine);
        reallyOffline = true;
    }

    if (client) {
        disconnect(client, &ClientSocket::signalConnected, this, &TcpMsgServer::SltConnected);
        disconnect(client, &ClientSocket::signalDisConnected, this, &TcpMsgServer::SltDisConnected);
        disconnect(client, &ClientSocket::signalLoginSuccess, this, &TcpMsgServer::SltLoginSuccess);
        disconnect(client, &ClientSocket::signalPrivateMsgToClient, this, &TcpMsgServer::SltPrivateMsgToClient);
        disconnect(client, &ClientSocket::signalGroupMsgToClient, this, &TcpMsgServer::SltGroupMsgToClient);
        disconnect(client, &ClientSocket::signalAddGroupMembers, this, &TcpMsgServer::SltAddGroupMembers);
        disconnect(client, &ClientSocket::signalRemoveGroupMembers, this, &TcpMsgServer::SltRemoveGroupMembers);
        disconnect(client, &ClientSocket::signalHeadChanged, this, &TcpMsgServer::SltHeadChangedToClient);
        client->deleteLater();
    }

    if (!reallyOffline && userId > 0) {
        emit signalServerLog(QString("用户 %1 的旧连接已断开（新连接保持在线，在线 %2 人）")
                             .arg(userName.isEmpty() ? QString::number(userId) : userName)
                             .arg(m_clientHash.size()));
    } else {
        emit signalServerLog(userName.isEmpty()
                             ? QString("一个客户端断开连接（在线 %1）").arg(m_clientHash.size())
                             : QString("用户 %1 已下线（在线 %2）").arg(userName).arg(m_clientHash.size()));
    }
    emit signalClientChanged(m_clientHash.size(), m_clients.size());
}

void TcpMsgServer::SltPublicMsgToClient(const int &id, const quint8 &type, const QJsonValue &json)
{

}

void TcpMsgServer::SltPrivateMsgToClient(const int &id, const int &targetId, const quint8 &type, const QJsonValue &jsonVal)
{
    if (targetId < 0)
    {
        qDebug() << "单播目标Id为空";
        return;
    }
    ClientSocket *targetClient = m_clientHash.value(QString::number(targetId), nullptr);
    if (!targetClient)
    {
        qDebug() << "[单播] 目标用户不存在或已下线：" << targetId;
        insertMessageQueue(id, targetId, jsonVal, type);
        return;
    }

    // 转发
    targetClient->SltSendMessage(type, QJsonValue(jsonVal));
    //    qDebug() << "server.cpp转发出去135" << jsonVal;
}

void TcpMsgServer::SltGroupMsgToClient(const int &id, const int &groupId, const quint8 &type, const QJsonValue &json)
{
    if(groupId < 0) {
        qDebug() << "组播目标出现错误";
        return;
    }
    qDebug() << "组播" << id << groupId << json;
    QSet<int> groupSet = m_groupMembersCache.value(groupId);

    // 缓存里没有这个群（例如成员是服务器运行期间直接写库加进来的、或缓存还没初始化），
    // 回退查一次数据库，否则消息会被静默丢掉，其它成员永远收不到
    if (groupSet.isEmpty())
    {
        QJsonValue members = DataBaseMag::getInstance()->getGroupMember(groupId);
        if (members.isArray())
        {
            const QJsonArray arr = members.toArray();
            for (const QJsonValue &item : arr)
            {
                const int memberId = item.toObject().value("userId").toInt();
                if (memberId > 0)
                {
                    groupSet.insert(memberId);
                }
            }
        }
        if (!groupSet.isEmpty())
        {
            m_groupMembersCache[groupId] = groupSet;
            qDebug() << "[群聊] 群组" << groupId << "从数据库补载成员：" << groupSet;
        }
    }

    if (groupSet.isEmpty())
    {
        qWarning() << "[群聊] 群组" << groupId
                   << "没有任何成员（GROUP_MEMBER 为空），消息无法转发给其他人";
        return;
    }

    QList<int> memberList = groupSet.toList();
    qDebug() << "[群聊] 群组" << groupId << "成员：" << memberList << " 发送者：" << id;

    // 群里只有发送者自己：没有别人可转发，这种多半是群成员没加上，日志里明确提示一下
    if (memberList.size() <= 1)
    {
        qWarning() << "[群聊] 群组" << groupId
                   << "只有" << memberList.size() << "个成员，其他人收不到该消息";
    }
    for (int i = 0; i < memberList.size(); i += BATCH_SIZE) {
        // 截取当前批次的成员ID
        QList<int> batch = memberList.mid(i, BATCH_SIZE);
        // 异步发送，不阻塞主线程
        QMetaObject::invokeMethod(this, [=]() {
            int batchSendCount = 0;
            int batchOfflineCount = 0;
            for (int userId : batch) {
                if (userId == id) continue;
                ClientSocket *client = m_clientHash.value(QString::number(userId), nullptr);
                if (client) {
                    SltPrivateMsgToClient(id, userId, type, json);
                    batchSendCount++;
                } else {
                    insertMessageQueue(id, userId, json, type);
                    batchOfflineCount++;
                }
            }
            qDebug() << QString("[群聊分批] 群组ID=%1 批次%2 成功=%3 离线=%4")
                        .arg(groupId).arg(i/BATCH_SIZE + 1).arg(batchSendCount).arg(batchOfflineCount);
        }, Qt::QueuedConnection);
    }

}

/**
 * @brief TcpMsgServer::SltHeadChangedToClient
 * 某个用户换了头像：通知所有其它在线用户，让他们把列表/聊天里的头像换过来
 */
void TcpMsgServer::SltHeadChangedToClient(const int &id, const QJsonValue &jsonVal)
{
    int notified = 0;
    for (auto it = m_clientHash.begin(); it != m_clientHash.end(); ++it)
    {
        if (!it.value() || it.key().toInt() == id) {
            continue;   // 跳过改头像的人自己
        }
        it.value()->SltSendMessage(HeadChanged, QJsonValue(jsonVal));
        ++notified;
    }
    qDebug() << "[头像] 已通知" << notified << "个在线用户：用户" << id << "换了头像";
}

void TcpMsgServer::SltLoginSuccess(ClientSocket *client, const QString &userId)
{
    if (!client || userId.isEmpty())
    {
        qDebug() << "客户端登录ID为空，拒绝绑定";
        client->deleteLater();
        return;
    }

    // 同一账号已有连接：顶号处理。
    // 以前这里是直接 return，导致新连接虽然登录成功却不在在线表里——
    // 界面上显示"在线"，实际收不到任何消息，发出去的消息全进了离线队列。
    ClientSocket *old = m_clientHash.value(userId, nullptr);
    if (old && old != client)
    {
        qInfo() << "[顶号] 用户" << userId << "重新登录，踢掉旧连接";
        m_clientHash.remove(userId);        // 先摘掉，旧连接迟到的断开回调就不会影响在线表
        m_clients.removeOne(old);
        old->kick();
    }

    m_clientHash.insert(userId, client);
    DataBaseMag::getInstance()->UpdateUserStatus(userId.toInt(), OnLine);
    sendUserMessageQueue(userId);

    const QString userName = DataBaseMag::getInstance()->getUsernameFromId(userId);
    qDebug() << "客户端登录成功，ID：" << userId << "，当前在线数：" << m_clientHash.size();
    emit signalServerLog(QString("用户 %1（ID:%2）在线，当前在线 %3 人")
                         .arg(userName.isEmpty() ? QStringLiteral("未知") : userName)
                         .arg(userId).arg(m_clientHash.size()));
    emit signalClientChanged(m_clientHash.size(), m_clients.size());
}

TcpFileServer::TcpFileServer(QObject *parent)
{
    m_threadPool = new QThreadPool(this);
    m_threadPool->setMaxThreadCount(10);
    m_threadPool->setExpiryTimeout(30000); // 线程空闲30秒回收

    m_fileSaveDir = MyApp::m_strRecvPath;
    QDir saveDir(m_fileSaveDir);
    if (!saveDir.exists())
    {
        saveDir.mkpath(".");
        qDebug() << "[TcpFileServer] 创建文件保存目录：" << m_fileSaveDir;
    }
}

TcpFileServer::~TcpFileServer()
{
    stop();
    // 清理线程池
    m_threadPool->clear();
    m_threadPool->waitForDone(5000);
}

bool TcpFileServer::StartListen(quint16 port)
{
    if (m_isRunning)
    {
        qWarning() << "[TcpFileServer] 服务器已运行";
        return true;
    }
    if (!this->listen(QHostAddress::Any, port))
    {
        qCritical() << "[TcpFileServer] 启动失败：" << this->errorString();
        return false;
    }

    m_isRunning = true;
    qInfo() << "[TcpFileServer] 启动成功，监听端口：" << port
            << "线程池最大线程数：" << m_threadPool->maxThreadCount();
    return true;
}

void TcpFileServer::stop()
{
    if (!m_isRunning)
        return;

    // 停止监听新连接
    this->close();
    m_isRunning = false;

    // 停止所有客户端任务
    QMutexLocker locker(&m_clientMutex);
    for (FileSocket *socket : m_clientMap.values())
    {
        socket->stopRecvFile(); // 停止文件接收
    }
    m_clientMap.clear();
    locker.unlock();

    qInfo() << "[TcpFileServer] 已停止，等待线程池任务完成...";
    m_threadPool->waitForDone(10000); // 等待10秒
    qInfo() << "[TcpFileServer] 完全停止";
}

void TcpFileServer::setFileSaveDir(const QString &dir)
{
    if (!dir.isEmpty())
    {
        m_fileSaveDir = dir;
        QDir saveDir(m_fileSaveDir);
        if (!saveDir.exists())
            saveDir.mkpath(".");
        qInfo() << "[TcpFileServer] 文件保存目录已更新：" << m_fileSaveDir;
    }
}

void TcpFileServer::setMaxThreadCount(int count)
{
    if (count > 0)
    {
        m_threadPool->setMaxThreadCount(count);
        qInfo() << "[TcpFileServer] 线程池最大线程数已更新：" << count;
    }
}

void TcpFileServer::incomingConnection(qintptr socketDescriptor)
{
    if (!m_isRunning)
    {
        qWarning() << "[TcpFileServer] 服务器已停止，拒绝新连接";
        // 关闭未处理的socket
        QTcpSocket tempSocket;
        tempSocket.setSocketDescriptor(socketDescriptor);
        tempSocket.disconnectFromHost();
        return;
    }

    // 创建 FileSocket 任务，并将描述符交给它以便在工作线程中创建 QTcpSocket
    FileSocket *fileSocket = new FileSocket();
    fileSocket->setSocketDescriptor(socketDescriptor);
    // 设置文件保存目录
    fileSocket->setSaveDir(m_fileSaveDir);

    // 使用临时键（socketDescriptor）先加入管理表，FileSocket 在运行时会获取真实 clientKey
    QString clientKey = QString::number(socketDescriptor);
    qInfo() << "[TcpFileServer] 新客户端连接（描述符）：" << clientKey;

    // 绑定FileSocket信号（跨线程传递到主线程）
    connect(fileSocket, &FileSocket::recvFinished, this, &TcpFileServer::onFileRecvFinished, Qt::QueuedConnection);
    connect(fileSocket, &FileSocket::taskFailed, this, &TcpFileServer::onTaskFailed, Qt::QueuedConnection);
    connect(fileSocket, &FileSocket::destroyed, this, [=]()
    {
        // FileSocket销毁时，从客户端列表移除（按指针匹配）
        QMutexLocker locker(&m_clientMutex);
        QString foundKey;
        for (auto it = m_clientMap.begin(); it != m_clientMap.end(); ++it) {
            if (it.value() == fileSocket) {
                foundKey = it.key();
                break;
            }
        }
        if (!foundKey.isEmpty()) {
            m_clientMap.remove(foundKey);
            emit clientDisconnected(foundKey);
            qInfo() << "[TcpFileServer] 客户端任务销毁：" << foundKey;
        } });

    // 加入客户端列表
    QMutexLocker locker(&m_clientMutex);
    m_clientMap.insert(clientKey, fileSocket);
    locker.unlock();

    // 提交到线程池执行
    m_threadPool->start(fileSocket);

    // 触发新连接信号
    emit clientConnected(clientKey);
}

void TcpFileServer::onFileRecvFinished(const QString &clientKey, const QString &filePath)
{
    qInfo() << "[TcpFileServer] 文件接收完成：" << clientKey << "→" << filePath;
    // 转发全局信号
    emit fileRecvFinished(clientKey, filePath);

    // 接收完成后移除客户端
    QMutexLocker locker(&m_clientMutex);
    // 尝试按 key 直接移除，否则按 value->clientKey() 匹配移除
    if (m_clientMap.contains(clientKey))
    {
        m_clientMap.remove(clientKey);
    }
    else
    {
        QString foundKey;
        for (auto it = m_clientMap.begin(); it != m_clientMap.end(); ++it)
        {
            if (it.value() && it.value()->clientKey() == clientKey)
            {
                foundKey = it.key();
                break;
            }
        }
        if (!foundKey.isEmpty())
            m_clientMap.remove(foundKey);
    }
}

void TcpFileServer::onTaskFailed(const QString &clientKey, const QString &error)
{
    qWarning() << "[TcpFileServer] 任务失败：" << clientKey << "→" << error;
    // 转发全局信号
    emit taskFailed(clientKey, error);

    // 任务失败，移除客户端
    QMutexLocker locker(&m_clientMutex);
    if (m_clientMap.contains(clientKey))
    {
        m_clientMap.remove(clientKey);
    }
    else
    {
        QString foundKey;
        for (auto it = m_clientMap.begin(); it != m_clientMap.end(); ++it)
        {
            if (it.value() && it.value()->clientKey() == clientKey)
            {
                foundKey = it.key();
                break;
            }
        }
        if (!foundKey.isEmpty())
            m_clientMap.remove(foundKey);
    }
}

void TcpFileServer::onClientDisconnected(const QString &clientKey)
{
    qInfo() << "[TcpFileServer] 客户端断开连接：" << clientKey;
    // 转发全局信号
    emit clientDisconnected(clientKey);
}

#include "mysocket.h"
#include "handshake.h"
#include "keystore.h"
#include <QJsonObject>
#include <qjsondocument.h>
#include <QHostAddress>
#include <QTimer>
#include "comapi/unit.h"
#include <comapi/myapp.h>
#include <comapi/cryptoutil.h>

static QString b64(const QByteArray &raw) { return QString::fromLatin1(raw.toBase64()); }
static QByteArray unb64(const QString &text) { return QByteArray::fromBase64(text.toLatin1()); }

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

    // 协议 v2：链路加密通道。握手在 TCP 连上之后立刻开始，
    // 完成之前业务报文只排队不发送——宁可慢一拍，也不让任何一个字节明文上网。
    m_channel = new SecureChannel(SecureChannel::Client, this);
    connect(m_channel, &SecureChannel::serverFingerprintLearned,
            this, [](const QByteArray &fp, const QString &pem) {
        // 首次连接：把服务器公钥固定下来（TOFU），之后每次都必须一致
        if (KeyStore::instance()->pinServerKey(pem)) {
            qInfo() << "[握手] 已固定服务器公钥，指纹" << fp.toHex().left(16);
        }
    });

    m_hsTimer = new QTimer(this);
    m_hsTimer->setSingleShot(true);
    m_hsTimer->setInterval(8000);
    connect(m_hsTimer, &QTimer::timeout, this, &MySocket::sltHandshakeTimeout);
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

        // 单帧上限 8 MiB：不校验的话，一个伪造的超大长度头会让缓冲区一直等到超时
        if (dataLen > 8u * 1024u * 1024u) {
            qWarning() << "[安全] 帧长度超限" << dataLen << "，断开";
            m_tcpSocket->abort();
            return;
        }

        if (m_recvBuffer.size() < 4 + dataLen) {
            break; // 数据未收全，等待下次
        }
        QByteArray frameBytes = m_recvBuffer.mid(4, dataLen);
        m_recvBuffer.remove(0, 4 + dataLen);

        QJsonDocument doc = QJsonDocument::fromJson(frameBytes);
        if (!doc.isObject()) {
            qWarning() << "[协议] 收到非 JSON 帧，丢弃";
            continue;
        }
        const QJsonObject frame = doc.object();

        // ---- 握手阶段：明文帧 ----
        if (frame.contains("hs")) {
            if (!handleHandshakeFrame(frame)) {
                m_tcpSocket->abort();
                return;
            }
            continue;
        }

        // ---- 数据面：没有加密通道就不接受任何业务数据 ----
        if (!m_channel->isReady()) {
            qWarning() << "[安全] 链路未加密却收到业务帧，断开";
            m_tcpSocket->abort();
            return;
        }

        QByteArray plain;
        QString err;
        if (!m_channel->open(frame, &plain, &err)) {
            qWarning() << "[安全]" << err << "，断开";
            m_tcpSocket->abort();
            return;
        }

        const QJsonDocument inner = QJsonDocument::fromJson(plain);
        if (!inner.isObject()) continue;

        const QJsonObject jsonObj = inner.object();
        const quint8 nType = static_cast<quint8>(jsonObj["type"].toInt());

        // 日志只留类型、长度和摘要。解开链路后这里就是完整明文，
        // 直接打到控制台等于把"谁发的、发了什么"写进本机日志文件。
        qDebug() << "客户端收到 type" << nType << "大小" << plain.size()
                 << "摘要" << QString::fromLatin1(CryptoUtil::sha256(plain).toHex().left(16));

        sendMsgType(nType, jsonObj["data"]);
    }
}

bool MySocket::handleHandshakeFrame(const QJsonObject &frame)
{
    QByteArray reply;
    QString err;
    if (!m_channel->handleHandshake(frame, &reply, &err)) {
        qWarning() << "[握手]" << err;
        emit signalConnectFailed(err);
        return false;
    }
    if (!reply.isEmpty() && !writeFrame(reply)) return false;

    if (m_channel->isReady()) {
        m_hsTimer->stop();
        qInfo() << "[握手] 链路已加密，可以开始业务通信";

        // 排队等着的报文（登录请求往往就在里面）现在一次性发出去
        flushOutQueue();

        emit signalSecureReady();
        emit signalConnectSuccess();

        if (m_bWasReconnect) {
            qInfo() << "[重连] 链路已恢复";
            emit signalReconnected();
        }
    }
    return true;
}

void MySocket::startHandshake()
{
    m_channel->reset();
    // 已固定的服务器公钥 -> 指纹比对；为空说明是首次连接（TOFU）
    const QString pinned = KeyStore::instance()->serverPubKey();
    m_channel->setPinnedServerPublicKey(pinned);

    const QByteArray ch = m_channel->clientHello();
    if (ch.isEmpty() || !writeFrame(ch)) {
        qWarning() << "[握手] 无法发起握手：" << m_channel->lastError();
        emit signalConnectFailed(m_channel->lastError());
        m_tcpSocket->abort();
        return;
    }
    m_hsTimer->start();
    qInfo() << "[握手] 已发出 CH" << (pinned.isEmpty() ? "（首次连接，将固定服务器指纹）" : "");
}

void MySocket::flushOutQueue()
{
    if (!m_channel->isReady()) return;
    while (!m_outQueue.isEmpty()) {
        const QByteArray plain = m_outQueue.takeFirst();
        QString err;
        const QByteArray sealed = m_channel->seal(plain, &err);
        if (sealed.isEmpty()) {
            qWarning() << "[安全] 排队报文加密失败：" << err;
            continue;
        }
        writeFrame(sealed);
    }
}

bool MySocket::writeFrame(const QByteArray &payload)
{
    if (!m_tcpSocket->isOpen() || payload.isEmpty()) return false;

    const quint32 dataLen = static_cast<quint32>(payload.size());
    QByteArray lenBytes(4, 0);
    lenBytes[0] = static_cast<char>((dataLen >> 24) & 0xFF);
    lenBytes[1] = static_cast<char>((dataLen >> 16) & 0xFF);
    lenBytes[2] = static_cast<char>((dataLen >> 8) & 0xFF);
    lenBytes[3] = static_cast<char>(dataLen & 0xFF);

    return m_tcpSocket->write(lenBytes + payload) > 0;
}

void MySocket::sltHandshakeTimeout()
{
    if (m_channel->isReady()) return;
    qWarning() << "[握手] 8 秒内未完成，判定链路不可信，断开";
    emit signalConnectFailed(tr("与服务器的加密握手超时，已断开"));
    m_tcpSocket->abort();
}

bool MySocket::isSecure() const
{
    return m_channel && m_channel->isReady();
}

void MySocket::sltConnect()
{
    m_recvBuffer.clear();
    m_bManualClose = false;
    m_nRetryCount = 0;
    stopReconnect();

    qDebug() << "已连接服务器:" << m_tcpSocket->peerAddress().toString()
             << m_tcpSocket->peerPort();

    m_bWasReconnect = m_bConnectedOnce;   // 之前连上过 = 这次是恢复
    m_bConnectedOnce = true;

    // 先握手，握手完成后才对外宣布"连上了"
    startHandshake();
}

void MySocket::sltDisconnect()
{
    qDebug() << "客户端断开连接";

    // 旧会话的密钥与排队报文一律作废：重连后重新握手、重新登录
    m_hsTimer->stop();
    m_channel->reset();
    m_outQueue.clear();
    m_loginPasswd.clear();

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
    QJsonObject json;
    json.insert("type",type);
    // 协议 v2：不再发送 from 字段。身份由登录绑定的连接决定，
    // 客户端自报家门这条路径本身就是身份伪造的入口，直接砍掉。
    json.insert("data",dataVal);

    QJsonDocument document;
    document.setObject(json);
    const QByteArray plain = document.toJson(QJsonDocument::Compact);

    // 大包（图片）只打印摘要，避免日志被 base64 淹没
    if (plain.size() <= 512)
        qDebug() << "客户端发送（明文，仅本地日志）：" << plain;
    else
        qDebug() << "客户端发送：大包" << plain.size() << "字节 type" << type;

    // 没连上就先发起连接，报文排队等握手完成——不再阻塞事件循环等 TCP 连上，
    // 否则握手报文在队列里排着，waitForConnected 只能干等到超时。
    if (m_tcpSocket->state() == QAbstractSocket::UnconnectedState) {
        connectToHost(MyApp::m_strHostAddr, MyApp::m_nMsgPort);
    }

    if (!m_channel->isReady()) {
        if (m_outQueue.size() < 256) m_outQueue.append(plain);
        else qWarning() << "[安全] 排队报文过多，丢弃最早的一条" << type;
        return;
    }

    QString err;
    const QByteArray sealed = m_channel->seal(plain, &err);
    if (sealed.isEmpty()) {
        qWarning() << "[安全] 加密待发报文失败：" << err;
        return;
    }
    writeFrame(sealed);
}

/**
 * @brief MySocket::sendLogin
 * 第一步只发用户名。口令留在本机，等服务器把 salt/迭代次数/nonce 送过来再算证明。
 */
void MySocket::sendLogin(const QString &name, const QString &passwd)
{
    m_loginName = name;
    m_loginPasswd = passwd;

    QJsonObject json;
    json.insert("name", name);
    json.insert("step", QStringLiteral("init"));
    sendMessage(Login, QJsonValue(json));
}

/**
 * @brief MySocket::handleAuthChallenge
 * 收到 0x11 Login 且 step == "challenge"：说明服务器在要口令证明。
 * 算完立刻把口令从内存里抹掉，只保留这一次性的 proof。
 */
bool MySocket::handleAuthChallenge(const QJsonValue &dataVal)
{
    if (!dataVal.isObject()) return false;
    const QJsonObject obj = dataVal.toObject();
    if (obj.value("step").toString() != QLatin1String("challenge")) return false;

    const QString alg = obj.value("alg").toString();
    const QByteArray salt = unb64(obj.value("salt").toString());
    const QByteArray nonce = unb64(obj.value("nonce").toString());
    const int iter = obj.value("iter").toInt();

    if (alg != QLatin1String("PBKDF2-SHA256") || salt.isEmpty() || nonce.size() != 32 || iter <= 0) {
        qWarning() << "[认证] 服务器下发的挑战参数不合法";
        emit signalStatus(LoginPasswdError, dataVal);
        return true;
    }
    if (m_loginPasswd.isEmpty() || m_loginName.isEmpty()) {
        qWarning() << "[认证] 没有待用的登录口令，忽略挑战";
        return true;
    }

    // verifier 就是服务端存的 digest；两边算出来的必须一模一样
    const QByteArray verifier = CryptoUtil::pbkdf2Sha256(m_loginPasswd.toUtf8(), salt, iter, 32);
    if (verifier.isEmpty()) {
        emit signalStatus(LoginPasswdError, dataVal);
        return true;
    }

    const QByteArray sid = m_channel->sessionId();
    QByteArray material("MC2/AUTH", 8);
    material.append(sid);
    material.append(nonce);
    material.append(m_loginName.toUtf8());

    QJsonObject out;
    out.insert("name", m_loginName);
    out.insert("step", QStringLiteral("proof"));
    out.insert("proof", b64(CryptoUtil::hmacSha256(verifier, material)));

    // 身份私钥签名：服务端登记过公钥时会验这一项，
    // 这样即使数据库泄露（能算出 proof），没有本机私钥也进不去。
    KeyStore *ks = KeyStore::instance();
    if (ks->isReady()) {
        QByteArray idInput("MC2/AUTH-ID", 11);
        idInput.append(sid);
        idInput.append(nonce);
        idInput.append(m_loginName.toUtf8());
        QByteArray sig;
        if (CryptoUtil::signDetached(ks->localPrivKey(), idInput, &sig))
            out.insert("sig", b64(sig));
        else
            qWarning() << "[认证] 身份私钥签名失败：" << CryptoUtil::lastError();
    }

    // 口令用完就丢，不在内存里常驻
    m_loginPasswd.clear();

    sendMessage(Login, QJsonValue(out));
    return true;
}

void MySocket::sendMsgType(const quint8 &nType, const QJsonValue &dataVal)
{
    // Login(0x11) 与 GroupOperateFailed(0x11) 数值撞车（历史上两套枚举混用），
    // 所以挑战必须在 switch 之前单独认：只有带 step=challenge 的才截获。
    if (nType == Login && dataVal.isObject()
            && dataVal.toObject().value("step").toString() == QLatin1String("challenge")) {
        handleAuthChallenge(dataVal);
        return;
    }

    switch (nType) {
    case LoginSuccess:
    {
        m_loginPasswd.clear();     // 登录成功后不再需要口令
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
        m_loginPasswd.clear();
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

#include "clientsocket.h"
#include "databasemag.h"
#include "myserver.h"
#include "handshake.h"
#include "comapi/unit.h"
#include "comapi/cryptoutil.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QImage>
#include <QDir>
#include <QMutex>
#include <QHash>
#include <QSet>
#include "comapi/myapp.h"

// ------------------------------------------------------------------
// 服务器身份密钥
//
// 实现在 handshake.cpp 的 ServerIdentity 里：它既给握手报文签名，
// 也给「公钥目录」签名，证明这份目录确实出自本服务器。
// 不参与任何消息加解密 —— 服务器依然不持有任何用户私钥。
// ------------------------------------------------------------------
static inline QString serverPrivPem() { return ServerIdentity::privateKeyPem(); }
static inline QString serverPubPem()  { return ServerIdentity::publicKeyPem();  }

// ------------------------------------------------------------------
// 口令枚举防护
//
// 挑战-应答本身要算一次 PBKDF2（10 万次迭代），攻击者每猜一次都得自己
// 付这份算力，等价于离线爆破成本；再加一层按用户名的失败计数，
// 把"在线猜"也堵掉。计数放内存，重启清零——毕设规模够用，
// 真要持久化应该落在账号表上（见文档 §15 局限）。
// ------------------------------------------------------------------
namespace {

QMutex g_authMutex;
QHash<QString, QPair<int, qint64>> g_authFail;   // 用户名 -> (连续失败次数, 锁到哪个时刻)

const int kMaxFailures = 5;
const qint64 kLockMs = 5 * 60 * 1000;

bool authLocked(const QString &name)
{
    QMutexLocker lock(&g_authMutex);
    const QPair<int, qint64> rec = g_authFail.value(name);
    return rec.first >= kMaxFailures && QDateTime::currentMSecsSinceEpoch() < rec.second;
}

void authNoteFailure(const QString &name)
{
    QMutexLocker lock(&g_authMutex);
    QPair<int, qint64> rec = g_authFail.value(name);
    ++rec.first;
    if (rec.first >= kMaxFailures)
        rec.second = QDateTime::currentMSecsSinceEpoch() + kLockMs;
    g_authFail.insert(name, rec);
    qWarning() << "[认证] 用户" << name << "认证失败" << rec.first << "次";
}

void authNoteSuccess(const QString &name)
{
    QMutexLocker lock(&g_authMutex);
    g_authFail.remove(name);
}

} // namespace



// 把客户端传来的图片统一裁剪成 96x96 的正方形头像
static QImage NormalizedHead(const QByteArray &raw)
{
    QImage img;
    if (!img.loadFromData(raw) || img.isNull()) {
        return QImage();
    }
    const int side = qMin(img.width(), img.height());
    if (side <= 0) {
        return QImage();
    }
    QImage square = img.copy((img.width() - side) / 2, (img.height() - side) / 2, side, side);
    return square.scaled(96, 96, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

ClientSocket::ClientSocket(QObject *parent, QTcpSocket *tcpSocket)
{
    m_nId = -1;
    m_lastActive = QDateTime::currentDateTime();

    m_tcpSocket = tcpSocket; // 优先使用传入的 Socket（服务器端场景）
    if (!m_tcpSocket) { // 只有传入为空时，才新建（客户端主动连接场景）
        m_tcpSocket = new QTcpSocket(this);
    }

    connect(m_tcpSocket, SIGNAL(readyRead()), this, SLOT(SltReadyRead()));
    connect(m_tcpSocket, SIGNAL(connected()), this, SLOT(SltConnected()));
    connect(m_tcpSocket, SIGNAL(disconnected()), this, SLOT(SltDisconnected()));

    // 协议 v2：每条连接自带一条加密通道，握手完成前不接受任何业务报文
    m_channel = new SecureChannel(SecureChannel::Server, this);
    m_channel->setServerPrivateKey(ServerIdentity::privateKeyPem());
    connect(m_channel, &SecureChannel::handshakeReady, this, [this]() {
        qInfo() << "[握手] 链路已加密，来自" << m_tcpSocket->peerAddress().toString();
    });
    connect(m_channel, &SecureChannel::handshakeFailed, this, [this](const QString &reason) {
        // 握手失败一律断链：不降级、不给"先明文聊着"的机会
        qWarning() << "[握手] 断开连接：" << reason;
        m_tcpSocket->abort();
    });
}

bool ClientSocket::isSecure() const
{
    return m_channel && m_channel->isReady();
}

ClientSocket::~ClientSocket()
{

}

int ClientSocket::GetUserId()
{
    return  m_nId;
}

/**
 * @brief 最近一次收到该连接数据的时刻，用于心跳超时判定
 */
QDateTime ClientSocket::lastActiveTime() const
{
    return m_lastActive;
}

/**
 * @brief 主动关闭连接（顶号时用），会触发 disconnected -> signalDisConnected
 */
void ClientSocket::kick()
{
    if (m_tcpSocket) {
        m_tcpSocket->disconnectFromHost();
        if (m_tcpSocket->state() != QAbstractSocket::UnconnectedState)
            m_tcpSocket->abort();
    }
}

void ClientSocket::Close()
{

}

void ClientSocket::sendMsgType(const quint8 &nType, const QJsonValue &dataVal)
{
    switch (nType) {
    case Login:
    {
        ParseLogin(dataVal);
        break;
    }
    case Register:
    {
        ParseReister(dataVal);
        break;
    }
    case AddFriend:
    {
        ParseAddFriend(dataVal);
        break;
    }
    case UserOnLine:    //用户上线
    {
        ParseUserOnline(dataVal);
        break;
    }
    case AddFriendReply:
    {
        ParseAddFriendReply(dataVal);
        break;
    }
    case RefreshFriends:
    {
        ParseRefreshFriend(dataVal);
        break;
    }
    case SendMsg:
    {
        ParseSendMsg(dataVal);
        break;
    }
    case SendFace:
    {
        ParseSendFace(dataVal);
        break;
    }
    case SendFile:
    {
        ParseSendFile(dataVal);
        break;
    }
    case DeleteFriend:
    {
        ParseDeleteMyFriend(dataVal);
        break;
    }
    case CreateGroup:
    {
        ParseCreateGroup(dataVal);
        break;
    }
    case RefreshGroups:
    {
        ParseRefreshGroups(dataVal);
        break;
    }
    case AddGroupRequist:
    {
        ParseAddGroupRequist(dataVal);
        break;
    }
    case AddGroupReply:
    {
        ParseAddGroupReply(dataVal);
        break;
    }
    case SendGroupMsg:
    {
        ParseSendGroupMsg(dataVal);
        break;
    }
    case RecallMsg:
    {
        ParseRecallMsg(dataVal);
        break;
    }
    case UpdateHeadPic:
    {
        ParseUpdateUserHead(dataVal);
        break;
    }
    case GetUserHead:
    {
        ParseGetUserHead(dataVal);
        break;
    }
    case QuitGroup:
    {
        ParseQuitGroup(dataVal);
        break;
    }
    case DissolveGroup:
    {
        ParseDissolveGroup(dataVal);
        break;
    }
    case GetGroupMembers:
    {
        ParseGetGroupMembers(dataVal);
        break;
    }
    case KickGroupMember:
    {
        ParseKickGroupMember(dataVal);
        break;
    }
    case TransferGroupOwner:
    {
        ParseTransferGroupOwner(dataVal);
        break;
    }
    case ChangePasswd:
    {
        ParseChangePasswd(dataVal);
        break;
    }
    case UploadKeys:
    {
        ParseUploadKeys(dataVal);
        break;
    }
    case GetPubKey:
    {
        ParseGetPubKey(dataVal);
        break;
    }
    case HeartBeat:
    {
        // 只要收到心跳就认为这条连接还活着，回一个应答让客户端知道链路正常
        m_lastActive = QDateTime::currentDateTime();
        QJsonObject ack;
        ack.insert("t", QDateTime::currentMSecsSinceEpoch());
        SltSendMessage(HeartBeatAck, QJsonValue(ack));
        break;
    }
    }
}

/**
 * @brief ClientSocket::ParseGetGroupMembers
 * 客户端拉取群成员列表：只有群成员才能看，返回昵称/头像/角色/在线状态
 */
void ClientSocket::ParseGetGroupMembers(const QJsonValue &dataVal)
{
    if (!dataVal.isObject()) return;

    const QJsonObject jsonObj = dataVal.toObject();
    const int groupId = jsonObj.value("groupId").toInt();
    if (groupId <= 0) {
        qDebug() << "[群成员] 群号无效";
        return;
    }

    const QJsonValue members = DataBaseMag::getInstance()->getGroupMemberDetail(groupId);

    // 非成员不给看：避免拿到别人的群成员名单
    bool isMember = false;
    const QJsonArray arr = members.toArray();
    for (const QJsonValue &item : arr) {
        if (item.toObject().value("userId").toInt() == m_nId) {
            isMember = true;
            break;
        }
    }
    if (!isMember) {
        QJsonObject errObj;
        errObj.insert("groupId", groupId);
        errObj.insert("msg", QString("你不在该群聊中，无法查看成员"));
        SltSendMessage(GroupOperateFailed, QJsonValue(errObj));
        return;
    }

    QJsonObject resObj;
    resObj.insert("groupId", groupId);
    resObj.insert("ownerId", DataBaseMag::getInstance()->getGroupOwner(groupId));
    resObj.insert("members", members);
    SltSendMessage(GroupMembers, QJsonValue(resObj));

    qDebug() << "[群成员] 已下发群" << groupId << "的成员列表，共" << arr.size() << "人";
}

/**
 * @brief ClientSocket::broadcastGroupMembersChanged
 * 成员变动后通知群里其它人刷新列表（组播会跳过操作者自己，离线成员进离线队列）
 */
void ClientSocket::broadcastGroupMembersChanged(int groupId, int ownerId)
{
    QJsonObject noticeObj;
    noticeObj.insert("groupId", groupId);
    noticeObj.insert("ownerId", ownerId);
    noticeObj.insert("operatorId", m_nId);
    emit signalGroupMsgToClient(m_nId, groupId, GroupMemberChanged, QJsonValue(noticeObj));
}

/**
 * @brief ClientSocket::ParseKickGroupMember
 * 群主把某人移出群聊：被踢的人收到 GroupKicked，其余成员收到成员变动通知
 */
void ClientSocket::ParseKickGroupMember(const QJsonValue &dataVal)
{
    if (!dataVal.isObject()) return;

    const QJsonObject jsonObj = dataVal.toObject();
    const int groupId  = jsonObj.value("groupId").toInt();
    const int targetId = jsonObj.value("userId").toInt();

    QJsonObject resObj;
    resObj.insert("groupId", groupId);
    resObj.insert("userId", targetId);

    auto fail = [this, &resObj](const QString &msg) {
        resObj.insert("msg", msg);
        SltSendMessage(GroupOperateFailed, QJsonValue(resObj));
        qDebug() << "[群成员] 移出失败：" << msg;
    };

    if (groupId <= 0 || targetId <= 0) {
        fail(QString("参数有误"));
        return;
    }
    if (targetId == m_nId) {
        fail(QString("不能把自己移出群聊，请使用退出群组"));
        return;
    }
    if (!DataBaseMag::getInstance()->isGroupOwner(groupId, m_nId)) {
        fail(QString("只有群主才能移出成员"));
        return;
    }
    if (DataBaseMag::getInstance()->isGroupOwner(groupId, targetId)) {
        fail(QString("不能移出群主"));
        return;
    }

    const QString groupName = DataBaseMag::getInstance()->getGroupName(groupId);
    if (!DataBaseMag::getInstance()->removeGroupMember(groupId, targetId)) {
        fail(QString("该成员不在群里或已被移出"));
        return;
    }

    // 通知被踢的人：在线立刻收到，离线则进队列，下次登录也会看到
    QJsonObject kickObj;
    kickObj.insert("groupId", groupId);
    kickObj.insert("name", groupName);
    kickObj.insert("operatorId", m_nId);
    emit signalPrivateMsgToClient(m_nId, targetId, GroupKicked, QJsonValue(kickObj));

    // 同步服务端的群成员缓存，之后的消息不再发给这个人
    emit signalRemoveGroupMembers(groupId, targetId);

    broadcastGroupMembersChanged(groupId, DataBaseMag::getInstance()->getGroupOwner(groupId));

    qInfo() << "[群成员] 用户" << m_nId << "把用户" << targetId
            << "移出了群" << groupId << groupName;

    resObj.insert("msg", "ok");
    SltSendMessage(GroupMemberChanged, QJsonValue(resObj));
}

/**
 * @brief ClientSocket::ParseTransferGroupOwner
 * 群主转让：原群主降为普通成员，目标成员升为群主，双方与群内其它人都收到通知
 */
void ClientSocket::ParseTransferGroupOwner(const QJsonValue &dataVal)
{
    if (!dataVal.isObject()) return;

    const QJsonObject jsonObj = dataVal.toObject();
    const int groupId  = jsonObj.value("groupId").toInt();
    const int targetId = jsonObj.value("userId").toInt();

    QJsonObject resObj;
    resObj.insert("groupId", groupId);
    resObj.insert("userId", targetId);

    auto fail = [this, &resObj](const QString &msg) {
        resObj.insert("msg", msg);
        SltSendMessage(GroupOperateFailed, QJsonValue(resObj));
        qDebug() << "[群成员] 转让失败：" << msg;
    };

    if (groupId <= 0 || targetId <= 0) {
        fail(QString("参数有误"));
        return;
    }
    if (targetId == m_nId) {
        fail(QString("你已经是群主了"));
        return;
    }
    if (!DataBaseMag::getInstance()->isGroupOwner(groupId, m_nId)) {
        fail(QString("只有群主才能转让群主"));
        return;
    }
    if (!DataBaseMag::getInstance()->getGroupMember(groupId).isArray()) {
        fail(QString("群成员数据异常"));
        return;
    }

    // 目标必须还在群里
    bool targetInGroup = false;
    const QJsonArray arr = DataBaseMag::getInstance()->getGroupMember(groupId).toArray();
    for (const QJsonValue &item : arr) {
        if (item.toObject().value("userId").toInt() == targetId) {
            targetInGroup = true;
            break;
        }
    }
    if (!targetInGroup) {
        fail(QString("该用户已不在群里"));
        return;
    }

    DataBaseMag::getInstance()->setGroupMemberIdentity(groupId, targetId, Owner);
    DataBaseMag::getInstance()->setGroupMemberIdentity(groupId, m_nId, Member);
    DataBaseMag::getInstance()->setGroupCreator(groupId, targetId);

    // 被转让的人单独收一条，确保他即使不在群里开会话也能感知
    QJsonObject noticeObj;
    noticeObj.insert("groupId", groupId);
    noticeObj.insert("name", DataBaseMag::getInstance()->getGroupName(groupId));
    noticeObj.insert("ownerId", targetId);
    noticeObj.insert("operatorId", m_nId);
    emit signalPrivateMsgToClient(m_nId, targetId, GroupMemberChanged, QJsonValue(noticeObj));

    broadcastGroupMembersChanged(groupId, targetId);

    qInfo() << "[群成员] 群" << groupId << "的群主由" << m_nId << "转让给" << targetId;

    resObj.insert("ownerId", targetId);
    resObj.insert("msg", "ok");
    SltSendMessage(GroupMemberChanged, QJsonValue(resObj));
}

/**
 * @brief ClientSocket::ParseChangePasswd
 * 修改密码：校验原密码 -> 更新 -> 回执
 */
void ClientSocket::ParseChangePasswd(const QJsonValue &dataVal)
{
    if (!dataVal.isObject()) {
        qDebug() << "修改密码数据有误";
        return;
    }

    const QJsonObject jsonObj = dataVal.toObject();
    const QString oldPwd = jsonObj.value("old").toString();
    const QString newPwd = jsonObj.value("new").toString();

    QJsonObject resObj;
    if (m_nId <= 0) {
        resObj.insert("msg", QString("尚未登录，无法修改密码"));
        SltSendMessage(ChangePasswdFailed, QJsonValue(resObj));
        return;
    }
    if (oldPwd.isEmpty() || newPwd.isEmpty()) {
        resObj.insert("msg", QString("原密码和新密码都不能为空"));
        SltSendMessage(ChangePasswdFailed, QJsonValue(resObj));
        return;
    }
    if (newPwd.length() < 6) {
        resObj.insert("msg", QString("新密码长度不能少于 6 位"));
        SltSendMessage(ChangePasswdFailed, QJsonValue(resObj));
        return;
    }

    const QString name = DataBaseMag::getInstance()->getUserInfoById(m_nId).value("name").toString();
    if (name.isEmpty()) {
        resObj.insert("msg", QString("账号不存在"));
        SltSendMessage(ChangePasswdFailed, QJsonValue(resObj));
        return;
    }
    if (!DataBaseMag::getInstance()->checkPassword(name, oldPwd)) {
        resObj.insert("msg", QString("原密码不正确"));
        SltSendMessage(ChangePasswdFailed, QJsonValue(resObj));
        return;
    }
    if (oldPwd == newPwd) {
        resObj.insert("msg", QString("新密码不能与原密码相同"));
        SltSendMessage(ChangePasswdFailed, QJsonValue(resObj));
        return;
    }
    if (!DataBaseMag::getInstance()->updatePassword(name, newPwd)) {
        resObj.insert("msg", QString("密码更新失败，请稍后重试"));
        SltSendMessage(ChangePasswdFailed, QJsonValue(resObj));
        return;
    }

    qDebug() << "用户" << name << "(" << m_nId << ") 修改密码成功";
    SltSendMessage(ChangePasswdOk, QJsonValue(resObj));
}

/**
 * @brief ClientSocket::ParseUploadKeys
 * 客户端上传自己的 RSA 公钥（供他人加密时使用）。
 * 公钥本身就是公开信息，服务器只把它登记成通讯录；私钥永远不出客户端。
 */
void ClientSocket::ParseUploadKeys(const QJsonValue &dataVal)
{
    QJsonObject resObj;
    if (!dataVal.isObject()) {
        resObj.insert("msg", QString("密钥数据格式错误"));
        SltSendMessage(KeyOperateFailed, QJsonValue(resObj));
        return;
    }
    if (m_nId <= 0) {
        resObj.insert("msg", QString("尚未登录，无法上传密钥"));
        SltSendMessage(KeyOperateFailed, QJsonValue(resObj));
        return;
    }

    const QJsonObject jsonObj = dataVal.toObject();
    const QString pubKey = jsonObj.value("pub").toString();

    if (pubKey.isEmpty() || !pubKey.contains("BEGIN PUBLIC KEY")) {
        resObj.insert("msg", QString("公钥格式不正确"));
        SltSendMessage(KeyOperateFailed, QJsonValue(resObj));
        return;
    }

    // 自签名校验：证明上传者确实持有对应私钥，
    // 否则别人可以先把自己的公钥登记成"某人的公钥"。
    const QString selfSig = jsonObj.value("selfSig").toString();
    const bool sigOk = !selfSig.isEmpty()
            && CryptoUtil::verifyDetached(pubKey,
                                          CryptoUtil::buildDirProof(m_nId, pubKey),
                                          QByteArray::fromBase64(selfSig.toLatin1()));
    if (!sigOk) {
        qWarning() << "[安全] 用户" << m_nId << "公钥自签名校验失败，拒绝登记";
        resObj.insert("msg", QString("公钥自签名校验失败：无法证明你持有对应私钥"));
        SltSendMessage(KeyOperateFailed, QJsonValue(resObj));
        return;
    }

    if (!DataBaseMag::getInstance()->updateUserKeys(m_nId, pubKey)) {
        resObj.insert("msg", QString("密钥保存失败"));
        SltSendMessage(KeyOperateFailed, QJsonValue(resObj));
        return;
    }

    qInfo() << "[加密] 用户" << m_nId << "上传公钥（服务器不保存任何私钥）";
    resObj.insert("msg", QString("ok"));
    resObj.insert("escrow", false);       // 兼容老客户端字段；本版本不存在密钥托管

    SltSendMessage(UploadKeysOk, QJsonValue(resObj));
}

/**
 * @brief ClientSocket::ParseGetPubKey
 * 请求用户公钥。data: {"ids":[1,2,3]} 或 {"id":5}；ids 为空表示"我的好友+同群成员"
 */
void ClientSocket::ParseGetPubKey(const QJsonValue &dataVal)
{
    DataBaseMag *db = DataBaseMag::getInstance();
    QList<int> ids;

    if (dataVal.isObject()) {
        const QJsonObject obj = dataVal.toObject();
        if (obj.contains("ids")) {
            const QJsonArray arr = obj.value("ids").toArray();
            for (int i = 0; i < arr.size(); ++i) ids.append(arr.at(i).toInt());
        } else if (obj.contains("id")) {
            ids.append(obj.value("id").toInt());
        }
    }

    QJsonObject keys;
    QJsonObject sigs;
    const QString spub  = serverPubPem();
    const QString spriv = serverPrivPem();
    int missing = 0;

    // 单次最多 200 条，且去重：公钥本身是公开信息，但没必要让人一次拉走整张表
    QList<int> unique;
    QSet<int> seen;
    foreach (int uid, ids) {
        if (uid <= 0 || seen.contains(uid)) continue;
        seen.insert(uid);
        unique.append(uid);
    }
    if (unique.size() > 200) {
        qWarning() << "[安全] 用户" << m_nId << "一次请求" << unique.size() << "个公钥，已拒绝";
        QJsonObject resObj;
        resObj.insert("msg", QString("一次最多查询 200 个公钥"));
        SltSendMessage(KeyOperateFailed, QJsonValue(resObj));
        return;
    }

    foreach (int uid, unique) {
        if (uid <= 0) continue;
        const QString pem = db->getUserPubKey(uid);
        if (pem.isEmpty()) { ++missing; continue; }
        keys.insert(QString::number(uid), pem);

        // 每条目录项都由服务器签名，客户端据此排除"目录被替换"
        if (!spriv.isEmpty()) {
            QByteArray sig;
            if (CryptoUtil::signDetached(spriv, CryptoUtil::buildDirProof(uid, pem), &sig))
                sigs.insert(QString::number(uid), QString::fromLatin1(sig.toBase64()));
        }
    }

    QJsonObject resObj;
    resObj.insert("keys", keys);
    resObj.insert("sigs", sigs);
    resObj.insert("spub", spub);
    resObj.insert("missing", missing);
    SltSendMessage(PubKeyData, QJsonValue(resObj));
}

void ClientSocket::writeFrame(const QByteArray &payload)
{
    if (!m_tcpSocket->isOpen() || payload.isEmpty()) return;

    // 统一用 4 字节大端长度头，防粘包；握手帧和密文帧走同一条通道
    const quint32 dataLen = static_cast<quint32>(payload.size());
    QByteArray lenBytes(4, 0);
    lenBytes[0] = static_cast<char>((dataLen >> 24) & 0xFF);
    lenBytes[1] = static_cast<char>((dataLen >> 16) & 0xFF);
    lenBytes[2] = static_cast<char>((dataLen >> 8) & 0xFF);
    lenBytes[3] = static_cast<char>(dataLen & 0xFF);

    m_tcpSocket->write(lenBytes + payload);
}

void ClientSocket::SltSendMessage(const quint8 &type, const QJsonValue &jsonVal)
{
    if (!m_tcpSocket->isOpen()) return;

    // 构建 Json 对象
    //
    // 这里原本还有一句 jsonObj.insert("from", m_nId)，填的是**本连接的用户 id**，
    // 既不是真正的发送者（发给 B 时填的是 B 自己的 id），客户端也从来没有读过它
    // （全工程 grep 不到 ["from"]）。留着只有副作用：每条报文（包括心跳）都带一个
    // 明文 uid，控制台一打印就把身份泄了。直接去掉。
    QJsonObject jsonObj;
    jsonObj.insert("type", type);
    jsonObj.insert("data", jsonVal);

    QJsonDocument document;
    document.setObject(jsonObj);
    const QByteArray plain = document.toJson(QJsonDocument::Compact);

    // 协议 v2：握手没完成就一个字节业务数据都不发。
    // 宁可让调用方"没收到回执"，也不能把明文吐到网络上。
    if (!isSecure()) {
        qWarning() << "[安全] 链路尚未加密，丢弃待发报文 type =" << type;
        return;
    }

    // 日志脱敏：plain 是解密后的完整明文（含收件人、消息体），
    // 打到控制台等于把"谁发给谁、发了什么"记在服务器本地。只留类型、长度和摘要，
    // 出了问题用摘要对得上就行，不需要留全文。
    qDebug() << "服务器发送 type" << type << "大小" << plain.size()
             << "摘要" << QString::fromLatin1(CryptoUtil::sha256(plain).toHex().left(16));

    QString err;
    const QByteArray sealed = m_channel->seal(plain, &err);
    if (sealed.isEmpty()) {
        qWarning() << "[安全] 加密待发报文失败：" << err;
        m_tcpSocket->abort();
        return;
    }
    writeFrame(sealed);
}

void ClientSocket::SltConnected()
{
    qDebug() << "服务器端socket检测到新的连接";
}

void ClientSocket::SltDisconnected()
{
    qDebug() << "服务器端socket检测到有用户下线";
    // 这里不直接改数据库状态：同一账号可能已经重新登录（新连接已在 hash 中），
    // 旧连接晚一步断开时如果把 status 置成 OffLine，会把在线的人标成离线。
    // 具体是否真的下线由 TcpMsgServer 根据连接表判定。
    Q_EMIT signalDisConnected(this);
}

void ClientSocket::SltReadyRead()
{
    m_lastActive = QDateTime::currentDateTime();
    QByteArray newData = m_tcpSocket->readAll();
    m_recvBuffer.append(newData);
    if (newData.size() <= 512)
        qDebug() << "服务器收到消息：" << newData;
    else
        qDebug() << "服务器收到消息：大包" << newData.size() << "字节";
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

        // ---- 握手阶段：明文帧，只认带 hs 的报文 ----
        if (frame.contains("hs")) {
            QByteArray reply;
            QString err;
            if (!m_channel->handleHandshake(frame, &reply, &err)) {
                qWarning() << "[握手] 处理失败：" << err;
                m_tcpSocket->abort();
                return;
            }
            if (!reply.isEmpty()) writeFrame(reply);
            continue;
        }

        // ---- 数据面：没握手就没资格发业务帧 ----
        if (!m_channel->isReady()) {
            qWarning() << "[安全] 未完成握手的连接直接发业务帧，断开";
            m_tcpSocket->abort();
            return;
        }

        QByteArray plain;
        QString err;
        if (!m_channel->open(frame, &plain, &err)) {
            qWarning() << "[安全] 链路帧解密失败：" << err << "；断开";
            m_tcpSocket->abort();
            return;
        }

        const QJsonDocument inner = QJsonDocument::fromJson(plain);
        if (!inner.isObject()) continue;

        const QJsonObject jsonObj = inner.object();
        const quint8 nType = static_cast<quint8>(jsonObj["type"].toInt());
        const QJsonValue dataVal = jsonObj["data"];

        // 协议 v2：身份只来自登录绑定的连接，报文里任何自称一律不认。
        // 未认证的连接只允许「注册 / 登录」两种报文，其余立即断开，
        // 否则任何人发一包 body 就能以别人的身份操作。
        if (m_nId <= 0 && nType != Login && nType != Register) {
            qWarning() << "[安全] 未认证连接发送业务报文 type =" << nType << "，断开";
            m_tcpSocket->abort();
            return;
        }

        sendMsgType(nType, dataVal);
    }


}

/**
 * @brief ClientSocket::ParseLogin
 * 协议 v2 的登录分两步走：
 *   ① 客户端只报用户名 -> 服务端回 salt / 迭代次数 / 一次性 nonce（挑战）
 *   ② 客户端用 PBKDF2 派生的 verifier 算 HMAC 证明（应答）
 * 口令原文永远不出客户端，链路上连哈希都不传，只有一次一用的 proof。
 */
void ClientSocket::ParseLogin(const QJsonValue &dataVal)
{
    if (!dataVal.isObject()) return;

    const QJsonObject dataObj = dataVal.toObject();
    if (dataObj.value("step").toString() == QLatin1String("proof")) {
        ParseLoginProof(dataObj);
        return;
    }
    ParseLoginInit(dataObj);
}

void ClientSocket::ParseLoginInit(const QJsonObject &dataObj)
{
    const QString name = dataObj.value("name").toString().trimmed();
    if (name.isEmpty()) {
        QJsonObject err;
        err.insert("msg", QStringLiteral("noneUser"));
        SltSendMessage(LoginPasswdError, QJsonValue(err));
        return;
    }

    int uid = -1;
    int iter = 0;
    QByteArray salt, verifier;
    const bool exists = DataBaseMag::getInstance()->authParams(name, &uid, &salt, &iter, &verifier);

    if (!exists) {
        // 用户名不存在时也给一个"稳定的假盐"：同一个名字每次挑战拿到同样的盐，
        // 攻击者没法靠"盐变来变去"判断这个号到底有没有注册过。
        salt = CryptoUtil::sha256(QByteArray("MC2/dummy-salt") + name.toUtf8()).left(16);
        iter = 100000;
        uid = -1;
        verifier.clear();
    }

    if (!CryptoUtil::random(32, &m_authNonce)) {
        QJsonObject err;
        err.insert("msg", QStringLiteral("noneUser"));
        SltSendMessage(LoginPasswdError, QJsonValue(err));
        return;
    }

    m_authName     = name;
    m_authUid      = uid;
    m_authSalt     = salt;
    m_authVerifier = verifier;

    QJsonObject out;
    out.insert("step", QStringLiteral("challenge"));
    out.insert("alg", QStringLiteral("PBKDF2-SHA256"));
    out.insert("salt", QString::fromLatin1(salt.toBase64()));
    out.insert("iter", iter);
    out.insert("nonce", QString::fromLatin1(m_authNonce.toBase64()));
    SltSendMessage(Login, QJsonValue(out));
}

void ClientSocket::ParseLoginProof(const QJsonObject &dataObj)
{
    const QString name = dataObj.value("name").toString().trimmed();

    // 没先要挑战、或名字与挑战时不一致：直接拒，不泄露"这个号存不存在"
    if (m_authNonce.isEmpty() || name != m_authName) {
        QJsonObject err;
        err.insert("msg", QStringLiteral("noneUser"));
        SltSendMessage(LoginPasswdError, QJsonValue(err));
        return;
    }

    if (authLocked(name)) {
        qWarning() << "[认证] 用户" << name << "连续失败次数过多，暂时锁定";
        QJsonObject err;
        err.insert("msg", QStringLiteral("locked"));
        SltSendMessage(LoginPasswdError, QJsonValue(err));
        m_authNonce.clear();
        return;
    }

    const QByteArray sid = m_channel ? m_channel->sessionId() : QByteArray();

    // proof = HMAC(verifier, "MC2/AUTH" ‖ sid ‖ nonce ‖ name)
    // sid 把证明绑死在这条链路上：换一条连接重放旧 proof 必然失败
    QByteArray material("MC2/AUTH", 8);
    material.append(sid);
    material.append(m_authNonce);
    material.append(name.toUtf8());

    const QByteArray want = CryptoUtil::hmacSha256(m_authVerifier, material);
    const QByteArray got  = QByteArray::fromBase64(dataObj.value("proof").toString().toLatin1());

    bool ok = !m_authVerifier.isEmpty() && !got.isEmpty()
            && CryptoUtil::constantTimeEquals(got, want);
    bool keyMismatch = false;

    // 若库里登记过身份公钥，再验一次私钥签名：
    // 这样即使数据库泄露（攻击者拿到 verifier 能自己算 proof），
    // 没有本机私钥依然进不来。首次登录尚未上传公钥时只验口令。
    if (ok && m_authUid > 0) {
        const QString pub = DataBaseMag::getInstance()->getUserPubKey(m_authUid);
        if (!pub.isEmpty()) {
            QByteArray idInput("MC2/AUTH-ID", 11);
            idInput.append(sid);
            idInput.append(m_authNonce);
            idInput.append(name.toUtf8());

            const QByteArray sig = QByteArray::fromBase64(dataObj.value("sig").toString().toLatin1());
            if (sig.isEmpty() || !CryptoUtil::verifyDetached(pub, idInput, sig)) {
                qWarning() << "[认证] 用户" << name << "身份私钥签名校验失败"
                           << "（本机密钥与服务端登记不一致，可能换过机器或重装过）";
                ok = false;
                keyMismatch = true;
            }
        }
    }

    // nonce 一次一用：无论成败立即作废，旧 proof 再送进来也对不上
    m_authNonce.clear();

    if (!ok) {
        authNoteFailure(name);
        QJsonObject err;
        err.insert("msg", keyMismatch ? QStringLiteral("keyMismatch")
                                      : QStringLiteral("noneUser"));
        SltSendMessage(LoginPasswdError, QJsonValue(err));
        return;
    }

    authNoteSuccess(name);
    qInfo() << "[认证] 用户" << name << "挑战-应答通过，uid =" << m_authUid;

    const int uid = m_authUid;
    m_authVerifier.clear();
    m_authSalt.clear();
    m_authUid = -1;

    completeLogin(DataBaseMag::getInstance()->loginResultById(uid));
}

void ClientSocket::completeLogin(QJsonObject jsonObj)
{
    m_nId = jsonObj.value("id").toInt();

    QString msg = jsonObj.value("msg").toString();

    // 数据库记着"在线"，但连接表里根本没有这个人 —— 说明上次是异常退出
    // （进程被杀/断网），状态没被回收。这种脏数据不能把用户永远挡在门外。
    if (msg == "OnLine") {
        const int realId = jsonObj.value("realId").toInt();
        TcpMsgServer *srv = TcpMsgServer::instance();
        const bool actuallyOnline = srv && srv->isUserOnline(realId);
        if (realId > 0 && !actuallyOnline) {
            qInfo() << "[登录] 用户" << realId << "数据库状态为在线但无实际连接，按离线放行";
            DataBaseMag::getInstance()->UpdateUserStatus(realId, OnLine);
            jsonObj.insert("id", realId);
            jsonObj.insert("msg", "ok");
            m_nId = realId;
            msg = "ok";
        }
    }

    if (m_nId > 0) emit signalConnected();

    if (msg == "ok") {  //不在线
        SltSendMessage(LoginSuccess, jsonObj);

        // 服务器不下发任何密钥材料：端到端加密要求服务器零知识，
        // 它只保管用户公钥，永远不持有能解开正文的私钥。
        emit signalLoginSuccess(this, QString::number(m_nId));
        qDebug() << "登陆成功 uid =" << m_nId;

        SltSendMessage(GetMyFriends, DataBaseMag::getInstance()->getMyFriends(m_nId));
        SltSendMessage(GetMyGroups, DataBaseMag::getInstance()->getMyGroup(m_nId));
        return;
    } else if (msg == "OnLine") {
        SltSendMessage(LoginRepeat, jsonObj);
    } else {
        SltSendMessage(LoginPasswdError, jsonObj);
    }
}

void ClientSocket::ParseUserOnline(const QJsonValue &dataVal)
{

}

void ClientSocket::ParseLogout(const QJsonValue &dataVal)
{

}

/**
 * @brief ClientSocket::ParseUpdateUserHead
 * 客户端上传头像：保存到服务器 UserHeads 目录 -> 更新数据库 -> 回执 -> 广播通知其它在线用户
 */
void ClientSocket::ParseUpdateUserHead(const QJsonValue &dataVal)
{
    if (!dataVal.isObject()) {
        qDebug() << "[头像] 上传数据格式有误";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();

    QJsonObject resObj;
    resObj.insert("id", m_nId);

    // 只能改自己的头像
    const int userId = jsonObj.value("id").toInt(m_nId);
    if (m_nId <= 0 || userId != m_nId) {
        resObj.insert("msg", QString("身份校验失败，无法修改头像"));
        SltSendMessage(HeadOperateFailed, QJsonValue(resObj));
        qWarning() << "[头像] 身份校验失败 client =" << m_nId << " 请求id =" << userId;
        return;
    }

    // 文件名只取 basename，避免目录穿越
    QString head = QFileInfo(jsonObj.value("head").toString()).fileName();
    if (head.isEmpty() || !head.endsWith(".png", Qt::CaseInsensitive)) {
        head = QString("head_%1_%2.png")
                   .arg(userId)
                   .arg(QDateTime::currentDateTime().toString("yyyyMMddHHmmsszzz"));
    }

    const QString base64 = jsonObj.value("data").toString();
    const QByteArray raw = QByteArray::fromBase64(base64.toLatin1());
    if (raw.size() < 64 || raw.size() > 4 * 1024 * 1024) {
        resObj.insert("msg", QString("头像数据大小异常（%1 字节）").arg(raw.size()));
        SltSendMessage(HeadOperateFailed, QJsonValue(resObj));
        return;
    }

    const QImage headImage = NormalizedHead(raw);
    if (headImage.isNull()) {
        resObj.insert("msg", QString("头像图片解析失败，请换一张图片"));
        SltSendMessage(HeadOperateFailed, QJsonValue(resObj));
        return;
    }

    const QString savePath = MyApp::HeadFullPath(head);
    QDir().mkpath(MyApp::m_strHeadPath);
    if (!headImage.save(savePath, "PNG")) {
        resObj.insert("msg", QString("头像保存失败：%1").arg(savePath));
        SltSendMessage(HeadOperateFailed, QJsonValue(resObj));
        qWarning() << "[头像] 保存失败：" << savePath;
        return;
    }

    if (!DataBaseMag::getInstance()->updateUserHead(userId, head)) {
        resObj.insert("msg", QString("头像更新失败，请稍后重试"));
        SltSendMessage(HeadOperateFailed, QJsonValue(resObj));
        return;
    }

    // 清掉这个用户以前上传的头像文件（不碰 default.png）
    const QStringList oldFiles = QDir(MyApp::m_strHeadPath)
                                     .entryList(QStringList() << QString("head_%1_*").arg(userId), QDir::Files);
    for (const QString &oldFile : oldFiles) {
        if (oldFile != head) {
            QFile::remove(MyApp::m_strHeadPath + oldFile);
        }
    }

    qInfo() << "[头像] 用户" << userId << "更新头像：" << head;

    resObj.insert("head", head);
    SltSendMessage(UpdateHeadOk, QJsonValue(resObj));

    // 广播给其它在线用户，让他们把头像换过来
    emit signalHeadChanged(userId, QJsonValue(resObj));
}

/**
 * @brief ClientSocket::ParseGetUserHead
 * 客户端本地缺头像文件时向服务器拉取图片内容
 */
void ClientSocket::ParseGetUserHead(const QJsonValue &dataVal)
{
    if (!dataVal.isObject()) {
        return;
    }
    QJsonObject reqObj = dataVal.toObject();
    const int userId = reqObj.value("id").toInt();
    if (userId <= 0) {
        return;
    }

    QString head = DataBaseMag::getInstance()->getUserHead(userId);
    QString path = MyApp::HeadFullPath(head);
    if (head.isEmpty() || !QFile::exists(path)) {
        // 数据库里没有、或文件不存在，统一回默认头像
        head = QStringLiteral("default.png");
        path = MyApp::EnsureDefaultHead();
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QJsonObject resObj;
        resObj.insert("id", userId);
        resObj.insert("msg", QString("服务器没有该用户的头像文件"));
        SltSendMessage(HeadOperateFailed, QJsonValue(resObj));
        qWarning() << "[头像] 读取头像失败：" << path;
        return;
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonObject resObj;
    resObj.insert("id", userId);
    resObj.insert("head", head);
    resObj.insert("data", QString::fromLatin1(data.toBase64()));
    SltSendMessage(HeadData, QJsonValue(resObj));

    qDebug() << "[头像] 下发头像给用户" << userId << "文件" << head << "大小" << data.size();
}

void ClientSocket::ParseReister(const QJsonValue &dataVal)
{
    if(dataVal.isObject()){
        QJsonObject jsonObj = dataVal.toObject();
        QString name = jsonObj.value("name").toString();
        QString passwd = jsonObj.value("passwd").toString();

        SltSendMessage(DataBaseMag::getInstance()->userRegister(name,passwd), jsonObj);
    }
}

//dataVal :senderId accessName
void ClientSocket::ParseAddFriend(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "添加好友传入参数有误";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
    QString friendName = jsonObj.value("name").toString();
    int senderId = jsonObj.value("id").toInt();
    //传输的obj先不管
    int friendId = DataBaseMag::getInstance()->getIdFromUsername(friendName);
    if(!DataBaseMag::getInstance()->haveUser(friendName)){
        SltSendMessage(AddFriendFailed_NoneUser,jsonObj);
        return;
    }
//    if(DataBaseMag::getInstance()->isFriend(GetUserId(),QString::number(friendId))){
//        SltSendMessage(AddFriendOk,jsonObj);
//        return;
//    }
    QString senderName = DataBaseMag::getInstance()->getUsernameFromId(QString::number(senderId));

//    qDebug() << jsonObj << "senderName" << senderName << "senderId" << senderId;
    QJsonObject resObj;

    resObj.insert("name",senderName);   //requestName
    resObj.insert("id",senderId);
    resObj.insert("targetId",friendId);
    QJsonValue resVal = resObj;

    emit signalPrivateMsgToClient(m_nId ,friendId, AddFriendRequist,resVal);
}

void ClientSocket::ParseAddFriendReply(const QJsonValue &dataVal)
{
//    resObj.insert("id",MyApp::m_nId); //
//    resObj.insert("name",name);       //这个名字是要转发给的对象的名字
//    resObj.insert("msg","refuse");
    if(!dataVal.isObject()){
        qDebug() << "添加好友回复转发失败";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
    int id = jsonObj.value("id").toInt();
    QString name = jsonObj.value("name").toString();
    QString msg = jsonObj.value("msg").toString();
    QJsonObject resObj;
    resObj.insert("id", id);
    resObj.insert("name",DataBaseMag::getInstance()->getUsernameFromId(QString::number(id)));
    resObj.insert("msg",msg);
    if(msg.compare("accept") == 0){
        DataBaseMag::getInstance()->addFriend(id, DataBaseMag::getInstance()->getIdFromUsername(name));
        DataBaseMag::getInstance()->addFriend(DataBaseMag::getInstance()->getIdFromUsername(name), id);
    }


    int sendId = DataBaseMag::getInstance()->getIdFromUsername(name);
    if(!DataBaseMag::getInstance()->isOnline(sendId)){
        DataBaseMag::getInstance()->insertMessageQueue(static_cast<int>(id),static_cast<int>(sendId),AddFriendReply, dataVal);
        return;
    }
    emit signalPrivateMsgToClient(m_nId, sendId,AddFriendReply, resObj);

    qDebug() << "套接字发送AddFriendReply信号clientsocket.cpp" << resObj;

}

void ClientSocket::ParseAddGroupRequist(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "添加群组失败,客户端传输的数据有误";
        return;
    }
//    json.insert("id", MyApp::m_nId);
//    json.insert("groupId", id.toInt());
    QJsonObject jsonObj = dataVal.toObject();
    int id = jsonObj.value("id").toInt();
    int groupId = jsonObj.value("groupId").toInt();
    QJsonObject resObj = jsonObj;
    resObj.insert("name", DataBaseMag::getInstance()->getUsernameFromId(QString::number(id)));  //要加群的人
    int targetId = DataBaseMag::getInstance()->getGroupOwner(groupId);  //群主Id

    emit signalPrivateMsgToClient(id, targetId, AddGroupRequist, QJsonValue(resObj));
}

void ClientSocket::ParseAddGroupReply(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()) {
        qDebug() << "添加群组回复出错，客户端传输的数据有误";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
    QString msg = jsonObj.value("msg").toString();
    int groupId = jsonObj.value("groupId").toInt();
    int to = jsonObj.value("to").toInt();
    int id = jsonObj.value("id").toInt();
    QJsonObject resObj;
    resObj.insert("groupId", groupId);
    resObj.insert("name", DataBaseMag::getInstance()->getUsernameFromId(QString::number(id)));    //群主名
    resObj.insert("groupName", DataBaseMag::getInstance()->getGroupName(groupId));
    resObj.insert("groupHead", DataBaseMag::getInstance()->getGroupHead(groupId));
    resObj.insert("id", id);    //群主Id
    resObj.insert("userName", DataBaseMag::getInstance()->getUsernameFromId(QString::number(to)));
    resObj.insert("userId", to);
    if(msg == QString("accept")) {                                                 //接受通知所有群成员
        DataBaseMag::getInstance()->addGroupMember(to, groupId, Member);
        resObj.insert("msg", QString("accept"));
        emit signalAddGroupMembers(groupId, to);
        signalPrivateMsgToClient(id, to, AddGroupAccept, DataBaseMag::getInstance()->getGroupMember(groupId));
        signalGroupMsgToClient(id, groupId, AddGroupReply, QJsonValue(resObj));
    } else if(msg == QString("refuse")) {                                         //拒绝只发给申请的用户
        resObj.insert("msg", QString("refuse"));
        signalPrivateMsgToClient(id, to, AddGroupReply, QJsonValue(resObj));
    }
}

void ClientSocket::ParseCreateGroup(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "创建群组失败，客户端传输的数据有误";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
    QString name = jsonObj.value("name").toString();
    int id = jsonObj.value("id").toInt();
    int groupId = DataBaseMag::getInstance()->addGroup(id, name);
    jsonObj.insert("groupId", groupId);
    SltSendMessage(CreateGroup, QJsonValue(jsonObj));
    emit signalAddGroupMembers(groupId, id);
}

void ClientSocket::ParseGetMyFriend(const QJsonValue &dataVal)
{
// 发送Jsonarray  status  head    name    id
}

void ClientSocket::ParseGetMyGroups(const QJsonValue &dataVal)
{

}

void ClientSocket::ParseDeleteMyFriend(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "删除好友操作错误";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
//    jsonObj.insert("id", MyApp::m_nId);
//      jsonObj.insert("to", cell->id);
    int id = jsonObj.value("id").toInt();
    int friendId = jsonObj.value("to").toInt();
    DataBaseMag::getInstance()->deleteFriend(id, friendId);
    DataBaseMag::getInstance()->deleteFriend(friendId, id);
    QJsonObject resObj;
    resObj.insert("id", id);
    emit signalPrivateMsgToClient(id, friendId, DeleteFriend, QJsonValue(resObj));
}

void ClientSocket::ParseRefreshFriend(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "客户端发送的刷新请求出错 dataVal = " << dataVal;
        return;

    }
    QJsonObject jsonObj = dataVal.toObject();
    int id = jsonObj.value("id").toInt();

    QJsonValue sendVal = DataBaseMag::getInstance()->getMyFriends(id);


    SltSendMessage(RefreshFriends, sendVal);
}

void ClientSocket::ParseRefreshGroups(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "客户端发送的刷新请求出错 dataVal = " << dataVal;
        return;

    }
    QJsonObject jsonObj = dataVal.toObject();
    int id = jsonObj.value("id").toInt();

    QJsonValue sendVal = DataBaseMag::getInstance()->getMyGroup(id);


    SltSendMessage(RefreshGroups, sendVal);
}

void ClientSocket::ParseFriendMessages(const QByteArray &reply)
{

}

void ClientSocket::ParseGroupMessages(const QByteArray &reply)
{

}

void ClientSocket::ParseFaceMessages(const QByteArray &reply)
{

}

/**
 * @brief ClientSocket::sendAck
 * 消息已被服务端受理：把 key 原样回给发送者。
 * 客户端据此把气泡从"发送中"改成"已送达"，超时未收到就标红提示重发。
 * 老客户端不带 key，这里直接跳过，不影响兼容。
 */
void ClientSocket::sendAck(const QJsonObject &jsonObj)
{
    const QString key = jsonObj.value("key").toString();
    if (key.isEmpty()) return;

    QJsonObject ack;
    ack.insert("key", key);
    ack.insert("type", jsonObj.value("type").toInt());
    SltSendMessage(SendAck, QJsonValue(ack));
}

void ClientSocket::ParseSendMsg(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "发送消息功能出错，收到的信息不是JsonObject";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
    const int targetId = jsonObj.value("id").toInt();

    // 授权校验：只能给好友发消息。缺了这一步，任何登录用户都能骚扰任何人。
    if (targetId <= 0 || !DataBaseMag::getInstance()->isFriendById(m_nId, targetId)) {
        qWarning() << "[授权] 用户" << m_nId << "试图向非好友" << targetId << "发送消息，已拒绝";
        QJsonObject rej;
        rej.insert("key", jsonObj.value("key").toString());
        rej.insert("msg", QString("对方不是你的好友，消息未发送"));
        SltSendMessage(SendRejected, QJsonValue(rej));
        return;
    }

    QJsonObject resObj;
    // v3：不再往转发报文里写发送者 uid。接收方从密文里解出身份，
    // 服务器写了也没意义（明文、可改、客户端也会覆盖），只会把身份泄在链路上。
    // 其余字段（msg/type/color/size/enc）仍一律原样透传。
    // 少带 type 接收端就会把图片当文字渲染成 base64，少带 color 自定义颜色也会丢。
    for (auto it = jsonObj.constBegin(); it != jsonObj.constEnd(); ++it)
    {
        const QString key = it.key();
        if (key == "id" || key == "to") continue;
        resObj.insert(key, it.value());
    }
    resObj.insert("head", DataBaseMag::getInstance()->getUserHead(m_nId));

//    qDebug() << "发送的head" <<  DataBaseMag::getInstance()->getUserHead(m_nId) << "\n";

    // 审计日志：整包原样落库。msg 字段是 v3 信封（hdr/iv/ct/tag/eks/sig），
    // 正文与收发双方 uid 都在密文里，服务器没有私钥，任何一天也解不开。
    // 这里记录的是消息类型（Text/Face/Files…），不是协议类型
    const quint8 msgType = static_cast<quint8>(jsonObj.value("type").toInt());
    DataBaseMag::getInstance()->insertMsgLog(
        m_nId, targetId, false, msgType,
        QString::fromUtf8(QJsonDocument(resObj).toJson(QJsonDocument::Compact)));

    emit signalPrivateMsgToClient(m_nId, targetId, SendMsg, QJsonValue(resObj));

    // 已受理，给发送者一个回执
    sendAck(jsonObj);
}

void ClientSocket::ParseSendFace(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "转发表情消息出错，客户端传输过来的不是JsonObj";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();

    // 原样透传（跳过 id/to，id 换成发送者），不要再手工重建 JSON：
    // key 是撤回时定位消息的唯一标识，丢了它对方就撤不掉；color/size 等同理。
    QJsonObject resObj;
    // 同上：发送者 uid 走密文，报文里不再出现
    for (auto it = jsonObj.constBegin(); it != jsonObj.constEnd(); ++it)
    {
        const QString key = it.key();
        if (key == "id" || key == "to") continue;
        // msg 字段是端到端密文，服务器不解析、不改写，原样透传
        resObj.insert(key, it.value());
    }
    resObj.insert("head", DataBaseMag::getInstance()->getUserHead(m_nId));

    emit signalPrivateMsgToClient(m_nId, jsonObj.value("id").toInt(),SendFace, QJsonValue(resObj));

    sendAck(jsonObj);
}

void ClientSocket::ParseSendFile(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()) {
        qDebug() << "转发文件出错，客户端传输来的不是JsonObj";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();

    // 同样整包透传：key（撤回定位）、size、type 一次带齐
    QJsonObject resObj;
    // 同上：发送者 uid 走密文，报文里不再出现
    for (auto it = jsonObj.constBegin(); it != jsonObj.constEnd(); ++it)
    {
        const QString key = it.key();
        if (key == "id" || key == "to") continue;
        resObj.insert(key, it.value());
    }
    resObj.insert("head", DataBaseMag::getInstance()->getUserHead(m_nId));

    emit signalPrivateMsgToClient(m_nId, jsonObj.value("to").toInt(), SendFile, QJsonValue(resObj));

    sendAck(jsonObj);
}

/**
 * @brief ClientSocket::ParseRecallMsg
 * 撤回通知：只带 msg_key，服务端不解析内容，按私聊/群聊转发给对端
 * 对端离线时和普通消息一样进离线队列，下次登录补投
 */
void ClientSocket::ParseRecallMsg(const QJsonValue &dataVal)
{
    if (!dataVal.isObject())
    {
        qDebug() << "撤回消息格式错误";
        return;
    }

    QJsonObject jsonObj = dataVal.toObject();
    const QString key = jsonObj.value("key").toString();
    if (key.isEmpty())
    {
        qDebug() << "[撤回] 缺少 key，忽略";
        return;
    }

    const bool isGroup = (jsonObj.value("group").toInt() != 0);
    const int targetId = jsonObj.value("id").toInt();

    QJsonObject resObj;
    resObj.insert("id", m_nId);     // 发起人
    resObj.insert("key", key);
    resObj.insert("group", isGroup ? 1 : 0);

    if (isGroup)
    {
        resObj.insert("gid", targetId);
        qDebug() << "[撤回] 用户" << m_nId << "撤回群" << targetId << "消息" << key;
        emit signalGroupMsgToClient(m_nId, targetId, RecallMsg, QJsonValue(resObj));
    }
    else
    {
        qDebug() << "[撤回] 用户" << m_nId << "撤回发给" << targetId << "的消息" << key;
        emit signalPrivateMsgToClient(m_nId, targetId, RecallMsg, QJsonValue(resObj));
    }
}

void ClientSocket::ParseSendGroupMsg(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "发送群组信息出错";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
    int type = jsonObj.value("type").toInt();
    int groupId = jsonObj.value("id").toInt();
    if(type == Files) {
        groupId = jsonObj.value("to").toInt();
    }

    // 授权校验：非群成员不能往群里发消息
    if (groupId <= 0 || !DataBaseMag::getInstance()->isGroupMember(groupId, m_nId)) {
        qWarning() << "[授权] 用户" << m_nId << "不是群" << groupId << "的成员，拒绝转发";
        QJsonObject rej;
        rej.insert("key", jsonObj.value("key").toString());
        rej.insert("msg", QString("你不在该群聊中，消息未发送"));
        SltSendMessage(SendRejected, QJsonValue(rej));
        return;
    }

    jsonObj.insert("head", DataBaseMag::getInstance()->getUserHead(m_nId));
    // 发送者 uid 不再写进报文：接收端从密文里解出后自己填 userId
    jsonObj.insert("name", DataBaseMag::getInstance()->getUsernameFromId(QString::number(m_nId)));
    qDebug() << type;

    // 审计日志：群聊同样只记录密文原文
    DataBaseMag::getInstance()->insertMsgLog(
        m_nId, groupId, true, static_cast<quint8>(type),
        QString::fromUtf8(QJsonDocument(jsonObj).toJson(QJsonDocument::Compact)));

    emit signalGroupMsgToClient(m_nId, groupId, SendGroupMsg, QJsonValue(jsonObj));

    sendAck(jsonObj);
}

/**
 * @brief ClientSocket::ParseQuitGroup
 * 退出群组：普通成员才能退，群主只能用"解散群聊"
 */
void ClientSocket::ParseQuitGroup(const QJsonValue &dataVal)
{
    if (!dataVal.isObject()) {
        qDebug() << "退出群组数据有误";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
    const int groupId = jsonObj.value("groupId").toInt();
    if (groupId < 0) {
        qDebug() << "退出群组失败，群号无效";
        return;
    }

    QJsonObject resObj;
    resObj.insert("groupId", groupId);

    const int ownerId = DataBaseMag::getInstance()->getGroupOwner(groupId);
    if (ownerId == m_nId) {
        resObj.insert("msg", QString("你是群主，不能退出群聊，请使用解散群聊"));
        SltSendMessage(GroupOperateFailed, QJsonValue(resObj));
        return;
    }

    if (!DataBaseMag::getInstance()->removeGroupMember(groupId, m_nId)) {
        resObj.insert("msg", QString("退出群组失败，请稍后重试"));
        SltSendMessage(GroupOperateFailed, QJsonValue(resObj));
        return;
    }

    // 同步服务端的群成员缓存
    emit signalRemoveGroupMembers(groupId, m_nId);

    qDebug() << "用户" << m_nId << "退出群组" << groupId;
    SltSendMessage(QuitGroupOk, QJsonValue(resObj));
}

/**
 * @brief ClientSocket::ParseDissolveGroup
 * 解散群组：只有群主可以，解散后通知所有成员
 */
void ClientSocket::ParseDissolveGroup(const QJsonValue &dataVal)
{
    if (!dataVal.isObject()) {
        qDebug() << "解散群组数据有误";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
    const int groupId = jsonObj.value("groupId").toInt();
    if (groupId < 0) {
        qDebug() << "解散群组失败，群号无效";
        return;
    }

    QJsonObject resObj;
    resObj.insert("groupId", groupId);

    if (DataBaseMag::getInstance()->getGroupOwner(groupId) != m_nId) {
        resObj.insert("msg", QString("只有群主才能解散群聊"));
        SltSendMessage(GroupOperateFailed, QJsonValue(resObj));
        return;
    }

    // 先通知其它成员（此时缓存还没删，组播才有对象）
    QJsonObject noticeObj;
    noticeObj.insert("groupId", groupId);
    noticeObj.insert("name", DataBaseMag::getInstance()->getGroupName(groupId));
    emit signalGroupMsgToClient(m_nId, groupId, GroupDissolved, QJsonValue(noticeObj));

    // 清缓存 + 删库
    QJsonValue members = DataBaseMag::getInstance()->getGroupMember(groupId);
    if (members.isArray()) {
        const QJsonArray arr = members.toArray();
        for (const QJsonValue &item : arr) {
            const int memberId = item.toObject().value("userId").toInt();
            if (memberId > 0) {
                emit signalRemoveGroupMembers(groupId, memberId);
            }
        }
    }
    DataBaseMag::getInstance()->removeGroup(groupId);

    qDebug() << "群组" << groupId << "已被群主" << m_nId << "解散";
    SltSendMessage(DissolveGroupOk, QJsonValue(resObj));
}

#include "keystore.h"

#include "comapi/cryptoutil.h"
#include "comapi/myapp.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRandomGenerator>

KeyStore *KeyStore::instance()
{
    static KeyStore *s = nullptr;
    if (!s) s = new KeyStore();
    return s;
}

KeyStore::KeyStore(QObject *parent) : QObject(parent)
{
    m_cacheFile = QDir(MyApp::m_strDataPath).filePath("Keys/peers.json");
    loadPeerCache();
}

bool KeyStore::isReady() const
{
    return !m_localPriv.isEmpty() && !m_localPub.isEmpty();
}

QString KeyStore::ensureLocalKeys(const QString &userName, int userId)
{
    Q_UNUSED(userId)

    // 已就绪且是同一个账号，直接复用（含重复调用）。
    //
    // 注意：必须比对 userName。私钥是按 <账号>.priv.pem 落盘的，
    // 而 KeyStore 是进程内单例 —— 如果只判 isReady()，用户在登录框里
    // 先输 A 再改成 B，第二次仍会拿 A 的私钥去签名，服务端用 B 登记的
    // 公钥验必然失败（keyMismatch）；更糟的是 B.priv.pem 永远不会被创建，
    // 下次重启客户端就会生成一对全新密钥，彻底对不上，只能管理员改库。
    if (isReady() && m_localUser == userName) return m_localPub;

    const QString dir = QDir(MyApp::m_strDataPath).filePath("Keys");
    QString pub;
    const QString priv = CryptoUtil::ensureKeyPair(dir, userName, &pub);
    if (priv.isEmpty() || pub.isEmpty()) {
        qWarning() << "[加密] 生成/读取本机密钥失败：" << CryptoUtil::lastError();
        return QString();
    }

    m_localPriv = priv;
    m_localPub  = pub;
    m_localUser = userName;

    qInfo() << "[加密] 本机密钥就绪，公钥长度" << pub.size();
    return m_localPub;
}

QString KeyStore::localPubKey() const
{
    return m_localPub;
}

QString KeyStore::localPrivKey() const
{
    return m_localPriv;
}

QByteArray KeyStore::randomSessionKey(int bytes)
{
    QByteArray out;
    if (bytes <= 0) return out;
    out.resize(bytes);
    QRandomGenerator *rng = QRandomGenerator::system();
    for (int i = 0; i < bytes; ++i)
        out[i] = static_cast<char>(rng->bounded(256));
    return out;
}

// ------------------------------------------------------------------
// 对端公钥缓存
// ------------------------------------------------------------------
bool KeyStore::cachePubKey(int userId, const QString &pem)
{
    if (userId <= 0 || pem.isEmpty()) return false;

    // 自己的公钥：换机/换密钥是正常操作，不做固定
    if (userId == MyApp::m_nId) {
        if (m_peerKeys.value(userId) != pem) {
            m_peerKeys.insert(userId, pem);
            savePeerCache();
        }
        return true;
    }

    const QString old = m_peerKeys.value(userId);
    if (old.isEmpty()) {
        m_peerKeys.insert(userId, pem);
        savePeerCache();
        qInfo() << "[加密] 首次记录用户" << userId << "公钥，指纹" << fingerprintOf(pem).left(19);
        return true;
    }

    if (old != pem) {
        // 关键防线：公钥说变就变，十有八九是服务器或被攻陷的中继在替换
        qWarning() << "[加密] 用户" << userId << "公钥与本地固定值不一致，已拒绝覆盖";
        emit signalPeerKeyConflict(userId, fingerprintOf(old), fingerprintOf(pem));
        return false;
    }
    return true;
}

bool KeyStore::confirmKeyChange(int userId, const QString &pem)
{
    if (userId <= 0 || pem.isEmpty()) return false;
    m_peerKeys.insert(userId, pem);
    savePeerCache();
    qInfo() << "[加密] 用户" << userId << "公钥已由用户确认更新，新指纹"
            << fingerprintOf(pem).left(19);
    return true;
}

QString KeyStore::fingerprintOf(const QString &pem)
{
    const QByteArray digest = CryptoUtil::sha256(pem.trimmed().toUtf8());
    if (digest.isEmpty()) return QString();
    const QString hex = QString::fromLatin1(digest.toHex()).toUpper();
    QStringList parts;
    for (int i = 0; i < hex.size(); i += 4)
        parts.append(hex.mid(i, 4));
    return parts.join(' ');
}

QString KeyStore::fingerprint(int userId) const
{
    return fingerprintOf(m_peerKeys.value(userId));
}

bool KeyStore::pinServerKey(const QString &pem)
{
    if (pem.trimmed().isEmpty()) return false;
    const QString file = QDir(MyApp::m_strDataPath).filePath("Keys/server.pub");

    if (m_serverPub.isEmpty()) {
        QFile f(file);
        if (f.open(QIODevice::ReadOnly)) {
            m_serverPub = QString::fromUtf8(f.readAll());
            f.close();
        }
    }

    if (m_serverPub.isEmpty()) {
        m_serverPub = pem;
        QDir().mkpath(QFileInfo(file).absolutePath());
        QFile f(file);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(pem.toUtf8());
            f.close();
        }
        qInfo() << "[加密] 首次固定服务器公钥，指纹" << fingerprintOf(pem).left(19);
        return true;
    }

    if (m_serverPub != pem) {
        qWarning() << "[加密] 服务器公钥与本地固定值不一致，公钥目录不可信";
        emit signalPeerKeyConflict(0, fingerprintOf(m_serverPub), fingerprintOf(pem));
        return false;
    }
    return true;
}

QString KeyStore::serverPubKey() const
{
    return m_serverPub;
}

QString KeyStore::pubKey(int userId) const
{
    return m_peerKeys.value(userId);
}

bool KeyStore::hasPubKey(int userId) const
{
    return !m_peerKeys.value(userId).isEmpty();
}

QList<int> KeyStore::missingKeys(const QList<int> &ids) const
{
    QList<int> out;
    foreach (int id, ids) {
        if (id <= 0) continue;
        if (m_peerKeys.value(id).isEmpty() && !out.contains(id)) out.append(id);
    }
    return out;
}

void KeyStore::loadPeerCache()
{
    QFile f(m_cacheFile);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    f.close();

    for (QJsonObject::const_iterator it = obj.constBegin(); it != obj.constEnd(); ++it) {
        const int uid = it.key().toInt();
        const QString pem = it.value().toString();
        if (uid > 0 && !pem.isEmpty()) m_peerKeys.insert(uid, pem);
    }
}

void KeyStore::savePeerCache()
{
    QJsonObject obj;
    for (QHash<int, QString>::const_iterator it = m_peerKeys.constBegin();
         it != m_peerKeys.constEnd(); ++it) {
        obj.insert(QString::number(it.key()), it.value());
    }
    QDir().mkpath(QFileInfo(m_cacheFile).absolutePath());
    QFile f(m_cacheFile);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    f.close();
}

// ------------------------------------------------------------------
// 加解密
// ------------------------------------------------------------------
bool KeyStore::canEncryptTo(const QList<int> &recipientIds) const
{
    if (!isReady() || recipientIds.isEmpty()) return false;
    foreach (int id, recipientIds) {
        if (id <= 0) return false;
        if (m_peerKeys.value(id).isEmpty()) return false;
    }
    return true;
}

QString KeyStore::encryptForV2(const QString &plain, const QList<int> &recipientIds, int senderId,
                               int toId, bool isGroup, int kind, const QString &msgKey,
                               QString *err)
{
    if (!isReady()) {
        if (err) *err = "本机密钥尚未就绪";
        return QString();
    }
    if (toId <= 0) {
        if (err) *err = "接收方无效";
        return QString();
    }

    QMap<int, QString> recipients;
    // 发送者自己也要有一份，便于本机回看历史记录
    if (senderId > 0 && hasPubKey(senderId)) recipients.insert(senderId, m_peerKeys.value(senderId));
    foreach (int id, recipientIds) {
        if (id <= 0 || recipients.contains(id)) continue;
        const QString pem = m_peerKeys.value(id);
        if (pem.isEmpty()) {
            if (err) *err = QString("缺少用户 %1 的公钥").arg(id);
            return QString();
        }
        recipients.insert(id, pem);
    }
    if (!recipients.contains(senderId) && !m_localPub.isEmpty())
        recipients.insert(senderId, m_localPub);

    MsgHeader hdr;
    hdr.mid   = randomSessionKey(16);          // 只求唯一，不需要保密
    hdr.from  = senderId;
    hdr.to    = toId;
    hdr.scope = isGroup ? QStringLiteral("group") : QStringLiteral("p2p");
    hdr.kind  = kind;
    hdr.ts    = QDateTime::currentSecsSinceEpoch();
    hdr.seq   = ++m_sendSeq;
    hdr.key   = msgKey;

    return CryptoUtil::encryptEnvelope(plain.toUtf8(), recipients, hdr, m_localPriv, err);
}

bool KeyStore::openEnvelope(const QString &packet, MsgHeader *hdr, QByteArray *raw,
                            QByteArray *sigInput, QByteArray *sig, QString *err)
{
    if (!isReady()) {
        if (err) *err = "本机密钥尚未就绪";
        return false;
    }

    QString reason;
    if (!CryptoUtil::openEnvelope(packet, hdr, MyApp::m_nId, m_localPriv,
                                  raw, sigInput, sig, &reason)) {
        if (err) *err = reason;
        return false;
    }
    return true;
}

bool KeyStore::verifyFrom(int from, const QByteArray &sigInput, const QByteArray &sig,
                          QString *err)
{
    if (from <= 0) {
        if (err) *err = "密文里的发送者标识无效";
        return false;
    }

    const QString pub = m_peerKeys.value(from);
    if (pub.isEmpty()) {
        if (err) *err = QString("缺少用户 %1 的公钥，无法验证来源").arg(from);
        return false;
    }

    if (!CryptoUtil::verifyDetached(pub, sigInput, sig)) {
        if (err) *err = QString("签名校验失败：消息不是用户 %1 发出的，或已被替换").arg(from);
        return false;
    }
    return true;
}

bool KeyStore::decryptAndVerify(const QString &packet, int outerFrom, int outerKind,
                                const QString &outerKey, QString *plain,
                                MsgHeader *hdrOut, QString *err)
{
    MsgHeader hdr;
    QByteArray raw, sigIn, sig;
    QString reason;

    if (!openEnvelope(packet, &hdr, &raw, &sigIn, &sig, &reason)) {
        if (err) *err = reason;
        return false;
    }

    // 外层字段与信封头必须一致：服务器按外层字段转发，被改过这里就会露馅
    if (outerFrom > 0 && outerFrom != hdr.from)
        reason = QString("外层发送者 %1 与信封内 %2 不一致").arg(outerFrom).arg(hdr.from);
    else if (outerKind >= 0 && outerKind != hdr.kind)
        reason = QString("外层消息类型 %1 与信封头 %2 不一致").arg(outerKind).arg(hdr.kind);
    else if (!outerKey.isEmpty() && outerKey != hdr.key)
        reason = "外层消息标识与信封头不一致";

    if (!reason.isEmpty()) {
        emit signalEnvelopeRejected(hdr.from, reason);
        if (err) *err = reason;
        return false;
    }

    // 防重放：同一条消息只处理一次
    const QString midKey = QString::fromLatin1(hdr.mid.toBase64());
    if (m_seenMid.contains(midKey)) {
        if (err) *err = "重复消息，已丢弃";
        return false;
    }

    if (!verifyFrom(hdr.from, sigIn, sig, &reason)) {
        emit signalEnvelopeRejected(hdr.from, reason);
        if (err) *err = reason;
        return false;
    }

    rememberMid(midKey);
    if (plain) *plain = QString::fromUtf8(raw);
    if (hdrOut) *hdrOut = hdr;
    return true;
}

void KeyStore::rememberMid(const QString &mid)
{
    if (mid.isEmpty() || m_seenMid.contains(mid)) return;
    m_seenMid.insert(mid);
    m_seenOrder.append(mid);
    while (m_seenOrder.size() > 4096) {
        m_seenMid.remove(m_seenOrder.takeFirst());
    }
}

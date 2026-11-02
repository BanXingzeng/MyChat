#include "keystore.h"

#include "comapi/cryptoutil.h"
#include "comapi/myapp.h"

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

    // 已经就绪（含重复调用）
    if (isReady()) return m_localPub;

    const QString dir = QDir(MyApp::m_strDataPath).filePath("Keys");
    QString pub;
    const QString priv = CryptoUtil::ensureKeyPair(dir, userName, &pub);
    if (priv.isEmpty() || pub.isEmpty()) {
        qWarning() << "[加密] 生成/读取本机密钥失败：" << CryptoUtil::lastError();
        return QString();
    }

    m_localPriv = priv;
    m_localPub  = pub;

    // 私钥文件尽量设为只读（Windows 下普通用户仍可读，属于演示级保护）
    QFile::setPermissions(QDir(dir).filePath(userName + ".priv.pem"),
                          QFile::ReadOwner | QFile::WriteOwner);

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
void KeyStore::cachePubKey(int userId, const QString &pem)
{
    if (userId <= 0 || pem.isEmpty()) return;
    const bool changed = (m_peerKeys.value(userId) != pem);
    m_peerKeys.insert(userId, pem);
    if (changed) savePeerCache();
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

QString KeyStore::encryptFor(const QString &plain, const QList<int> &recipientIds, int senderId,
                             QString *err)
{
    if (!isReady()) {
        if (err) *err = "本机密钥尚未就绪";
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

    if (!recipients.contains(senderId)) {
        // 服务器没回自己的公钥时，用本机公钥补齐
        if (!m_localPub.isEmpty()) recipients.insert(senderId, m_localPub);
    }

    QString packet = CryptoUtil::encryptMessage(plain, recipients);
    if (packet.isEmpty() && err) *err = CryptoUtil::lastError();
    return packet;
}

bool KeyStore::decrypt(const QString &packet, QString *plain, QString *err)
{
    if (!isReady()) {
        if (err) *err = "本机密钥尚未就绪";
        return false;
    }
    return CryptoUtil::decryptMessage(packet, m_localPriv, MyApp::m_nId, plain, err);
}

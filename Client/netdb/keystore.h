#ifndef KEYSTORE_H
#define KEYSTORE_H

#include <QObject>
#include <QString>
#include <QList>
#include <QHash>
#include <QMap>

/**
 * @brief 客户端密钥中心
 *
 * 职责：
 *  1. 管理本机 RSA 密钥对（私钥只落在本机 Data/Keys/<user>.priv.pem，绝不上传、永不外传）；
 *  2. 缓存通讯对方的公钥（会话打开时按需向服务器索取，公钥本就是公开信息）；
 *  3. 发送时把正文加密成密文包，接收时用本机私钥解密；
 *  4. 为文件传输生成一次性会话密钥，并用各接收者公钥包裹后随文件头下发。
 *
 * 安全边界：服务器只保管公钥，不持有任何私钥副本，也不存在"管理员托管私钥"，
 * 因此服务器侧无法解密任何消息正文与文件内容。
 */
class KeyStore : public QObject
{
    Q_OBJECT
public:
    static KeyStore *instance();

    // ===== 初始化 =====
    bool isReady() const;                 // 本机密钥是否就绪
    /// 载入或生成本机密钥对；返回公钥 PEM（失败返回空）
    QString ensureLocalKeys(const QString &userName, int userId);

    QString localPubKey() const;
    QString localPrivKey() const;

    // ===== 对端公钥缓存 =====
    void cachePubKey(int userId, const QString &pem);
    QString pubKey(int userId) const;
    bool hasPubKey(int userId) const;
    /// 从 ids 里挑出还没有公钥的用户
    QList<int> missingKeys(const QList<int> &ids) const;

    // ===== 加解密 =====
    QString encryptFor(const QString &plain, const QList<int> &recipientIds, int senderId,
                       QString *err = nullptr);
    bool decrypt(const QString &packet, QString *plain, QString *err = nullptr);

    /// 是否已具备给这些用户发加密消息的条件
    bool canEncryptTo(const QList<int> &recipientIds) const;

    // ===== 文件传输会话密钥 =====
    /// 生成一次性的 AES-256 会话密钥 + IV（key 32 字节，iv 16 字节）
    static QByteArray randomSessionKey(int bytes);

private:
    explicit KeyStore(QObject *parent = nullptr);

    void loadPeerCache();
    void savePeerCache();

private:
    QString m_localPriv;
    QString m_localPub;
    QHash<int, QString> m_peerKeys;
    QString m_cacheFile;
};

#endif // KEYSTORE_H

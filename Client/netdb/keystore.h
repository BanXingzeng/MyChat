#ifndef KEYSTORE_H
#define KEYSTORE_H

#include <QObject>
#include <QString>
#include <QList>
#include <QHash>
#include <QMap>
#include <QSet>

#include "comapi/cryptoutil.h"

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
    /// 记录对端公钥。首次写入返回 true；若与已固定指纹不一致则**不覆盖**、返回 false
    /// 并由 signalPeerKeyConflict 通知界面人工确认（防止服务器替换公钥）
    bool cachePubKey(int userId, const QString &pem);
    /// 用户确认"新公钥可信"后强制覆盖
    bool confirmKeyChange(int userId, const QString &pem);
    QString pubKey(int userId) const;
    bool hasPubKey(int userId) const;
    /// 公钥指纹（SHA-256，十六进制分组），用于人工核对
    static QString fingerprintOf(const QString &pem);
    QString fingerprint(int userId) const;
    /// 固定服务器公钥：首次记录，之后必须一致，否则目录数据一律不采信
    bool pinServerKey(const QString &pem);
    QString serverPubKey() const;
    /// 从 ids 里挑出还没有公钥的用户
    QList<int> missingKeys(const QList<int> &ids) const;

    // ===== 加解密 =====
    /// v2：加密 + 用本机私钥签名。toId 为接收者 uid 或群 id
    QString encryptForV2(const QString &plain, const QList<int> &recipientIds, int senderId,
                         int toId, bool isGroup, int kind, const QString &msgKey,
                         QString *err = nullptr);
    /// v3：解开信封，把发送者/接收者从密文里解出来。
    /// 验签材料一并返回 —— 公钥要等 from 解出来才查得到，所以拆成两步。
    bool openEnvelope(const QString &packet, MsgHeader *hdr, QByteArray *raw,
                      QByteArray *sigInput, QByteArray *sig, QString *err = nullptr);
    /// 用 from 对应的公钥验证上一步拿到的签名材料
    bool verifyFrom(int from, const QByteArray &sigInput, const QByteArray &sig,
                    QString *err = nullptr);
    /// 解密 + 验签 + 防重放。outerFrom/outerKind/outerKey 来自外层 JSON，
    /// 会与信封头交叉校验，不一致说明中继动过手脚
    bool decryptAndVerify(const QString &packet, int outerFrom, int outerKind,
                          const QString &outerKey, QString *plain,
                          MsgHeader *hdr = nullptr, QString *err = nullptr);

    /// 是否已具备给这些用户发加密消息的条件
    bool canEncryptTo(const QList<int> &recipientIds) const;

    // ===== 文件传输会话密钥 =====
    /// 生成一次性的 AES-256 会话密钥 + IV（key 32 字节，iv 16 字节）
    static QByteArray randomSessionKey(int bytes);

signals:
    /// 对方公钥与本地记录不一致：可能被服务器或中间人替换，必须人工核对
    void signalPeerKeyConflict(int userId, const QString &oldFp, const QString &newFp);
    /// 收到一条签名/防重放校验不通过的消息
    void signalEnvelopeRejected(int fromId, const QString &reason);

private:
    explicit KeyStore(QObject *parent = nullptr);

    void loadPeerCache();
    void savePeerCache();
    void rememberMid(const QString &mid);

private:
    QString m_localPriv;
    QString m_localPub;
    QString m_localUser;                // 当前这对密钥属于哪个账号
    QString m_serverPub;
    QHash<int, QString> m_peerKeys;
    QString m_cacheFile;

    quint64 m_sendSeq = 0;              // 发送序号，单调递增
    QSet<QString> m_seenMid;            // 本次会话已处理的消息 ID
    QList<QString> m_seenOrder;         // 控制 m_seenMid 容量
};

#endif // KEYSTORE_H

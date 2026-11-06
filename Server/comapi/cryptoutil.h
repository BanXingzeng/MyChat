#ifndef CRYPTOUTIL_H
#define CRYPTOUTIL_H

#include <QString>
#include <QByteArray>
#include <QMap>

/**
 * @brief 端到端加密工具（OpenSSL 4.1.0 / Win32）
 *
 * 设计要点：
 *  1. 正文用 AES-256-GCM 加密（带认证标签，防篡改）；
 *  2. 每条消息随机生成一把 AES 密钥，用每个收件人的 RSA-2048 公钥
 *     （OAEP-SHA256）分别包裹，放在 eks（encrypted keys）里；
 *  3. 服务器只接触密文包，不持有任何用户私钥 → 无法解密正文；
 *  4. 发送者自己也在 eks 中，便于本机会话回看；
 *  5. 本模块只提供「封装」能力，不提供任何「托管/代解」入口。
 *
 * 密文包（紧凑 JSON，作为消息 msg 字段的字符串存放）：
 *  {"v":1,"alg":"AES-256-GCM","iv":"<b64>","ct":"<b64>","tag":"<b64>","eks":{"<uid>":"<b64>"}}
 */
class CryptoUtil
{
public:
    /// OpenSSL 运行库版本，例如 "OpenSSL 4.1.0 ..."
    static QString version();

    /// 最近一次失败原因（无错误时为空）
    static QString lastError();
    static void clearError();

    // ================= 密钥对 =================
    static bool generateKeyPair(int bits, QString *privPem, QString *pubPem);
    static bool isValidPrivateKey(const QString &privPem);
    static QString publicKeyFromPrivate(const QString &privPem);

    // ================= 密钥文件 =================
    /// 读取（不存在则生成）dir/<user>.priv.pem，同时返回对应公钥 PEM
    static QString ensureKeyPair(const QString &dir, const QString &userName, QString *pubPem);
    static bool savePrivateKey(const QString &filePath, const QString &privPem);
    static QString loadPrivateKey(const QString &filePath);

    // ================= RSA-OAEP 包裹 =================
    static bool wrap(const QString &pubPem, const QByteArray &data, QByteArray *out);
    static bool unwrap(const QString &privPem, const QByteArray &data, QByteArray *out);

    // ================= 消息包 =================
    /// recipients：用户ID -> 公钥PEM（必须包含发送者自己）
    static QString encryptMessage(const QString &plain, const QMap<int, QString> &recipients);
    /// 用本机私钥解密，myId 用于从 eks 里取自己那份包裹密钥
    static bool decryptMessage(const QString &packet, const QString &privPem, int myId,
                               QString *plain, QString *err = nullptr);
    /// 判断一段文本是否是本模块产生的密文包
    static bool isPacket(const QString &text);
    /// 取密文包里包裹密钥的接收者列表（审计/调试用；v3 起 eks 不标 uid，恒为空）
    static QList<int> packetRecipients(const QString &packet);
    /// 密文包里包裹了几份密钥（= 有几个收件人能解开）。v3 下这是服务器唯一能看到的收件信息
    static int packetKeyCount(const QString &packet);

    // ================= 数字签名（RSA-PSS-SHA256） =================
    static QByteArray sha256(const QByteArray &data);
    static bool signDetached(const QString &privPem, const QByteArray &data, QByteArray *sig);
    static bool verifyDetached(const QString &pubPem, const QByteArray &data, const QByteArray &sig);
    /// 公钥目录条目签名输入，客户端用同一函数重算后验签
    static QByteArray buildDirProof(int uid, const QString &pubPem);

    // ================= 链路层原语（P2 握手） =================
    /// 密码学随机数
    static bool random(int len, QByteArray *out);
    /// HMAC-SHA256
    static QByteArray hmacSha256(const QByteArray &key, const QByteArray &data);
    /// PBKDF2-HMAC-SHA256；参数与 PasswordHash 一致，输出即库里的 verifier
    static QByteArray pbkdf2Sha256(const QByteArray &pass, const QByteArray &salt,
                                   int iterations, int outLen);
    /// 定长比较：差在第几位耗时都一样
    static bool constantTimeEquals(const QByteArray &a, const QByteArray &b);

    /// X25519 临时密钥对（私钥、公钥各 32 字节，用完即弃）
    static bool generateX25519KeyPair(QByteArray *priv, QByteArray *pub);
    static QByteArray x25519PublicFromPrivate(const QByteArray &priv);
    /// 共享密钥；对端送低阶点（导出全零）时返回 false
    static bool x25519SharedSecret(const QByteArray &priv, const QByteArray &peerPub,
                                   QByteArray *secret);

    /// HKDF-SHA256：Extract / Expand 分开暴露，便于按 RFC 5869 独立复核
    static QByteArray hkdfExtract(const QByteArray &salt, const QByteArray &ikm);
    static QByteArray hkdfExpand(const QByteArray &prk, const QByteArray &info, int len);

    /// 单次 AES-256-GCM（非流式）；ct 与明文等长，tag 固定 16 字节
    static bool aesGcmSeal(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
                           const QByteArray &plain, QByteArray *ct, QByteArray *tag);
    static bool aesGcmOpen(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
                           const QByteArray &ct, const QByteArray &tag, QByteArray *plain);
};

#endif // CRYPTOUTIL_H

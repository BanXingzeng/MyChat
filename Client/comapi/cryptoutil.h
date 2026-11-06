#ifndef CRYPTOUTIL_H
#define CRYPTOUTIL_H

#include <QString>
#include <QByteArray>
#include <QMap>

/**
 * @brief v2 消息信封头
 *
 * 这几个字段全部参与 AAD 计算，任何一位被改动都会导致 GCM 校验失败；
 * from / to / kind / ts / seq / mid 另外参与签名，防止第三方伪造来源。
 */
struct MsgHeader
{
    QByteArray mid;             // 16 字节消息唯一标识，防重放
    int        from = 0;        // 发送者 uid
    int        to = 0;          // 接收者 uid 或群 id
    QString    scope = "p2p";   // p2p / group
    int        kind = 0;        // MessageType：Text/Audio/Picture/Files/Face
    qint64     ts = 0;          // Unix 秒
    quint64    seq = 0;         // 发送者本地单调序号
    QString    key;             // 撤回定位键（原外层 key，挪进来后受签名保护）
};

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
    /// 取密文包里包裹密钥的接收者列表（审计/调试用）
    static QList<int> packetRecipients(const QString &packet);

    // ================= 数字签名（RSA-PSS-SHA256） =================
    static QByteArray sha256(const QByteArray &data);
    static bool signDetached(const QString &privPem, const QByteArray &data, QByteArray *sig);
    static bool verifyDetached(const QString &pubPem, const QByteArray &data, const QByteArray &sig);

    // ================= v2 消息信封 =================
    /// 头部 -> AAD（参与 GCM，防篡改）
    static QByteArray buildAad(const MsgHeader &hdr);
    /// 公钥目录条目签名输入：服务器对它签名，客户端据此确认公钥没被替换
    static QByteArray buildDirProof(int uid, const QString &pubPem);
    /// 加密并签名，返回信封 JSON；失败返回空串
    static QString encryptEnvelope(const QByteArray &plain, const QMap<int, QString> &recipients,
                                   const MsgHeader &hdr, const QString &senderPrivPem,
                                   QString *err = nullptr);
    /// 只解析信封外层头（不解密、不验签）。
    /// v3 起 from/to 在密文里，这里恒为 0，要身份请用 openEnvelope。
    static bool peekEnvelope(const QString &packet, MsgHeader *hdr, QString *err = nullptr);
    /// 解密信封并把发送者/接收者从密文里解出来；
    /// 验签材料通过 sigInput/sigOut 返回，由调用方查公钥后自行 verifyDetached ——
    /// 因为"用哪个公钥"取决于解出来的 from，而 from 只有在解密后才知道。
    static bool openEnvelope(const QString &packet, MsgHeader *hdr, int myId,
                             const QString &myPrivPem, QByteArray *plain,
                             QByteArray *sigInput, QByteArray *sigOut,
                             QString *err = nullptr);

    // ================= 链路层原语（P2 握手） =================
    /// 密码学随机数
    static bool random(int len, QByteArray *out);
    /// HMAC-SHA256
    static QByteArray hmacSha256(const QByteArray &key, const QByteArray &data);
    /// PBKDF2-HMAC-SHA256；参数与 PasswordHash 一致，输出即服务端存的 verifier
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

/**
 * @brief AES-256-CTR 流式加解密（文件内容专用）
 *
 * 为什么用 CTR 而不是 GCM：
 *  - CTR 是流密码，密文长度严格等于明文长度，可以任意分片 update，
 *    不需要 padding，也不依赖"一次拿到全量数据"——大文件才能边读边传；
 *  - GCM 需要等全部密文到齐才能校验 tag，不适合逐块落盘的场景。
 *
 * 完整性由外层文件消息承载：密钥包裹在 RSA-OAEP 里，篡改会导致解密报错。
 */
class CryptoStream
{
public:
    CryptoStream();
    ~CryptoStream();

    CryptoStream(const CryptoStream &) = delete;
    CryptoStream &operator=(const CryptoStream &) = delete;

    /// key 必须是 32 字节，iv 必须是 16 字节
    bool initEncrypt(const QByteArray &key, const QByteArray &iv);
    bool initDecrypt(const QByteArray &key, const QByteArray &iv);

    bool isActive() const;
    /// 输入多少字节输出多少字节
    QByteArray update(const QByteArray &in);
    QByteArray finish();

private:
    void reset();

    void *m_ctx;
    bool m_ok;
};

#endif // CRYPTOUTIL_H

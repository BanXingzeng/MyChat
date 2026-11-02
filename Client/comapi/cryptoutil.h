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
    /// 取密文包里包裹密钥的接收者列表（审计/调试用）
    static QList<int> packetRecipients(const QString &packet);
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

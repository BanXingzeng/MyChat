#ifndef HANDSHAKE_H
#define HANDSHAKE_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

/**
 * @brief 链路加密通道（协议 v2 的 L1 层）
 *
 * 解决的问题：P1 只保证了「消息是谁发的」，但整条 TCP 链路仍是明文——
 * 口令裸奔、收件人 uid 可见、任何人都能掐断或劫持连接。这一层把
 * 客户端与服务器之间的每一帧都装进 AEAD 里，并把服务器身份钉死。
 *
 * 设计取舍（答辩要点）：
 *  1. 不用 TLS：Qt 5.14 的 QSslSocket 只认 OpenSSL 1.1.1，工程链的是 4.1，
 *     版本对不上；改用 1-RTT 手写握手，思路与 TLS 1.3 一致；
 *  2. 没有 CA：自签场景下用「服务器公钥指纹本地固定」（TOFU）当信任锚；
 *  3. X25519 临时密钥每次连接重新生成，用完立即清零 —— 历史流量事后无法解密；
 *  4. 握手报文里的 nonce / 临时公钥都被服务器签名覆盖，中间人改一位就断链。
 *
 * 报文（明文，握手阶段）：
 *   CH {"hs":"CH","ver":2,"nonce_c","eph_c","algs","cid"}
 *   SH {"hs":"SH","ver":2,"nonce_s","eph_s","sid","cert":{uid,pub,fp},"sig"}
 *   CF {"hs":"CF","fin":"<HMAC(kc2s, ...)>"}
 *   SF {"hs":"SF","fin":"<HMAC(ks2c, ...)>"}
 *
 * 数据帧（握手完成后）：{"c":"<base64(密文‖16字节tag)>"}
 *   nonce = 4 字节 0 ‖ 8 字节大端序号；AAD = "MC2/REC/v2" ‖ 方向 ‖ 序号。
 *   序号不复用、不重排（TCP 保序），解密失败即断链，不做任何"容错降级"。
 */
class SecureChannel : public QObject
{
    Q_OBJECT
public:
    enum Role { Client, Server };
    enum Status {
        Idle,           // 尚未开始
        Handshaking,    // 握手中
        Ready,          // 加密通道已就绪
        Failed          // 握手失败，调用方必须断链
    };

    explicit SecureChannel(Role role, QObject *parent = nullptr);

    // ---------- 配置 ----------
    /// 服务端：身份私钥（给 SH 签名）。不设置则 SH 无法生成。
    void setServerPrivateKey(const QString &privPem);
    /// 客户端：本地固定的服务器公钥指纹（32 字节）。为空表示首次连接（TOFU）
    void setPinnedServerFingerprint(const QByteArray &fp);
    /// 客户端：直接给本地固定的公钥 PEM（内部算 SPI 摘要；空串=首次连接）
    void setPinnedServerPublicKey(const QString &pubPem);
    /// 客户端：握手完成后学到的服务器公钥（供上层落盘）
    QString learnedServerPublicKey() const { return m_learnedPub; }
    QByteArray serverFingerprint() const { return m_serverFp; }

    // ---------- 状态 ----------
    Status status() const { return m_status; }
    bool isReady() const { return m_status == Ready; }
    QByteArray sessionId() const { return m_sid; }
    QByteArray transcriptHash() const { return m_transcript; }
    QString lastError() const { return m_lastError; }
    /// 重连时彻底丢弃旧会话（密钥、序号全部清零）
    void reset();

    // ---------- 客户端 ----------
    /// 生成并返回 CH 帧（明文 JSON）。失败返回空。
    QByteArray clientHello();

    // ---------- 握手帧处理 ----------
    /**
     * 处理一帧握手报文。
     * @param frame   收到的 JSON 报文（含 "hs" 字段）
     * @param reply   需要回发的帧（可能为空）
     * @return true 表示这一帧被正常处理（不代表已经 Ready，要看 status()）
     */
    bool handleHandshake(const QJsonObject &frame, QByteArray *reply, QString *err = nullptr);

    // ---------- 数据面 ----------
    /// 明文 JSON -> 密文帧 JSON；未就绪返回空并给出原因
    QByteArray seal(const QByteArray &plain, QString *err = nullptr);
    /// 密文帧 JSON -> 明文；失败即视为链路被破坏
    bool open(const QJsonObject &frame, QByteArray *plain, QString *err = nullptr);

signals:
    void handshakeReady();
    void handshakeFailed(const QString &reason);
    /// 客户端首次连接：把学到的服务器公钥交给上层固定（TOFU）
    void serverFingerprintLearned(const QByteArray &fp, const QString &pubPem);

private:
    bool onClientHello(const QJsonObject &frame, QByteArray *reply);
    bool onServerHello(const QJsonObject &frame, QByteArray *reply);
    bool onClientFinish(const QJsonObject &frame, QByteArray *reply);
    bool onServerFinish(const QJsonObject &frame, QByteArray *reply);

    /// 计算 TRANS 并签名（服务端）/ 重算并验签（客户端）
    QByteArray buildTranscript(const QByteArray &fp) const;
    /// X25519 + HKDF，派生 kc2s / ks2c，然后立即销毁临时私钥
    bool deriveSessionKeys();
    bool checkFinish(const QJsonObject &frame, const QByteArray &key);

    bool fail(const QString &reason);
    void becomeReady();

private:
    Role       m_role;
    Status     m_status;
    QString    m_lastError;

    QString    m_serverPriv;      // 服务端身份私钥
    QByteArray m_pinnedFp;        // 客户端固定指纹（32B，空=首次）
    QByteArray m_serverFp;        // 本次握手算出的服务器指纹
    QString    m_learnedPub;      // 客户端学到的服务器公钥 PEM

    QByteArray m_ephPriv;         // 本端 X25519 临时私钥（派生出密钥后清零）
    QByteArray m_ephC;            // 客户端临时公钥
    QByteArray m_ephS;            // 服务端临时公钥
    QByteArray m_nonceC;
    QByteArray m_nonceS;
    QByteArray m_sid;             // 16 字节会话 ID
    QByteArray m_transcript;      // H(TRANS)

    QByteArray m_kc2s;
    QByteArray m_ks2c;
    quint64    m_sendSeq;
    quint64    m_recvSeq;
};

#endif // HANDSHAKE_H

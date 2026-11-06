#ifndef HANDSHAKE_H
#define HANDSHAKE_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

/**
 * @brief 服务器身份密钥
 *
 * 只做一件事：给 SH 里的握手参数签名，证明"对面确实是这台服务器"。
 * 它不参与任何消息加解密，服务器依然不持有任何用户私钥。
 * 首次启动自动生成并落在 Data/Keys/server.priv.pem，之后不可删除或更换；
 * 一旦更换，所有客户端都会报"服务器身份指纹不一致"并拒绝连接——
 * 这是设计如此，不是故障。
 */
namespace ServerIdentity
{
QString privateKeyPem();
QString publicKeyPem();
QByteArray fingerprint();
}

/**
 * @brief 链路加密通道（协议 v2 的 L1 层，服务端侧）
 *
 * 与客户端共用同一份状态机和字节规则，差异只有两处：
 *   - 服务端持有身份私钥，负责生成 SH 并对 TRANS 签名；
 *   - 服务端不固定对端指纹（它面对的是任意客户端），
 *     客户端身份由后续的挑战-应答 + 消息签名来确认。
 */
class SecureChannel : public QObject
{
    Q_OBJECT
public:
    enum Role { Client, Server };
    enum Status {
        Idle,
        Handshaking,
        Ready,
        Failed
    };

    explicit SecureChannel(Role role, QObject *parent = nullptr);

    void setServerPrivateKey(const QString &privPem);
    void setPinnedServerFingerprint(const QByteArray &fp);
    void setPinnedServerPublicKey(const QString &pubPem);
    QString learnedServerPublicKey() const { return m_learnedPub; }
    QByteArray serverFingerprint() const { return m_serverFp; }

    Status status() const { return m_status; }
    bool isReady() const { return m_status == Ready; }
    QByteArray sessionId() const { return m_sid; }
    QByteArray transcriptHash() const { return m_transcript; }
    QString lastError() const { return m_lastError; }
    void reset();

    QByteArray clientHello();
    bool handleHandshake(const QJsonObject &frame, QByteArray *reply, QString *err = nullptr);

    QByteArray seal(const QByteArray &plain, QString *err = nullptr);
    bool open(const QJsonObject &frame, QByteArray *plain, QString *err = nullptr);

signals:
    void handshakeReady();
    void handshakeFailed(const QString &reason);
    void serverFingerprintLearned(const QByteArray &fp, const QString &pubPem);

private:
    bool onClientHello(const QJsonObject &frame, QByteArray *reply);
    bool onServerHello(const QJsonObject &frame, QByteArray *reply);
    bool onClientFinish(const QJsonObject &frame, QByteArray *reply);
    bool onServerFinish(const QJsonObject &frame, QByteArray *reply);

    QByteArray buildTranscript(const QByteArray &fp) const;
    bool deriveSessionKeys();
    bool checkFinish(const QJsonObject &frame, const QByteArray &key);

    bool fail(const QString &reason);
    void becomeReady();

private:
    Role       m_role;
    Status     m_status;
    QString    m_lastError;

    QString    m_serverPriv;
    QByteArray m_pinnedFp;
    QByteArray m_serverFp;
    QString    m_learnedPub;

    QByteArray m_ephPriv;
    QByteArray m_ephC;
    QByteArray m_ephS;
    QByteArray m_nonceC;
    QByteArray m_nonceS;
    QByteArray m_sid;
    QByteArray m_transcript;

    QByteArray m_kc2s;
    QByteArray m_ks2c;
    quint64    m_sendSeq;
    quint64    m_recvSeq;
};

#endif // HANDSHAKE_H

#include "handshake.h"

#include "comapi/cryptoutil.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QtDebug>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace {

const char kVer = 2;

QString b64(const QByteArray &raw)
{
    return QString::fromLatin1(raw.toBase64());
}

QByteArray unb64(const QString &text)
{
    return QByteArray::fromBase64(text.toLatin1());
}

/// SHA256(SubjectPublicKeyInfo DER)：这就是"公钥指纹"的定义，
/// 与 openssl x509 -pubkey | openssl pkey -pubin -outform DER | sha256 的结果一致
QByteArray spkiDigest(const QString &pubPem)
{
    if (pubPem.trimmed().isEmpty()) return QByteArray();
    const QByteArray raw = pubPem.toUtf8();
    BIO *bio = BIO_new_mem_buf(raw.constData(), raw.size());
    if (!bio) return QByteArray();
    EVP_PKEY *key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!key) return QByteArray();

    QByteArray digest;
    const int len = i2d_PUBKEY(key, nullptr);
    if (len > 0) {
        QByteArray der(len, 0);
        unsigned char *p = reinterpret_cast<unsigned char *>(der.data());
        if (i2d_PUBKEY(key, &p) == len)
            digest = CryptoUtil::sha256(der);
    }
    EVP_PKEY_free(key);
    return digest;
}

/// AAD：把方向和序号钉进认证范围，防止把上行的帧原样反射回来当下行用
QByteArray recordAad(bool fromClient, quint64 seq)
{
    QByteArray aad("MC2/REC/v2", 10);
    aad.append(static_cast<char>(fromClient ? 1 : 2));
    for (int i = 7; i >= 0; --i)
        aad.append(static_cast<char>((seq >> (i * 8)) & 0xFF));
    return aad;
}

/// 12 字节 nonce：高 4 字节固定 0，低 8 字节是单调序号
QByteArray recordNonce(quint64 seq)
{
    QByteArray nonce(12, 0);
    for (int i = 0; i < 8; ++i)
        nonce[4 + i] = static_cast<char>((seq >> ((7 - i) * 8)) & 0xFF);
    return nonce;
}

QByteArray finishInput(const QByteArray &transcript)
{
    return QByteArray("MC2/fin", 7) + transcript;
}

} // namespace

SecureChannel::SecureChannel(Role role, QObject *parent)
    : QObject(parent)
    , m_role(role)
    , m_status(Idle)
    , m_sendSeq(1)
    , m_recvSeq(1)
{
}

void SecureChannel::setServerPrivateKey(const QString &privPem)
{
    m_serverPriv = privPem;
}

void SecureChannel::setPinnedServerFingerprint(const QByteArray &fp)
{
    m_pinnedFp = fp;
}

void SecureChannel::setPinnedServerPublicKey(const QString &pubPem)
{
    // 上层存的是公钥 PEM（Data/Keys/server.pub），这里换成 32 字节指纹再比对
    m_pinnedFp = pubPem.trimmed().isEmpty() ? QByteArray() : spkiDigest(pubPem);
}

void SecureChannel::reset()
{
    m_status = Idle;
    m_lastError.clear();
    m_ephPriv.clear();
    m_ephC.clear();
    m_ephS.clear();
    m_nonceC.clear();
    m_nonceS.clear();
    m_sid.clear();
    m_transcript.clear();
    m_kc2s.clear();
    m_ks2c.clear();
    m_serverFp.clear();
    m_learnedPub.clear();
    m_sendSeq = 1;
    m_recvSeq = 1;
}

bool SecureChannel::fail(const QString &reason)
{
    m_status = Failed;
    m_lastError = reason;
    qWarning() << "[握手] 失败：" << reason;
    emit handshakeFailed(reason);
    return false;
}

void SecureChannel::becomeReady()
{
    m_status = Ready;
    qInfo() << "[握手] 加密通道就绪，会话" << m_sid.toHex().left(8) << "方向密钥已派生";
    emit handshakeReady();
}

// ------------------------------------------------------------------
// 客户端：CH
// ------------------------------------------------------------------
QByteArray SecureChannel::clientHello()
{
    if (m_role != Client) {
        fail("只有客户端能发起 CH");
        return QByteArray();
    }
    reset();

    QByteArray cid;
    if (!CryptoUtil::generateX25519KeyPair(&m_ephPriv, &m_ephC)
            || !CryptoUtil::random(32, &m_nonceC)
            || !CryptoUtil::random(16, &cid)) {
        fail(QString("生成握手随机数失败：%1").arg(CryptoUtil::lastError()));
        return QByteArray();
    }

    QJsonObject obj;
    obj.insert("hs", QStringLiteral("CH"));
    obj.insert("ver", int(kVer));
    obj.insert("nonce_c", b64(m_nonceC));
    obj.insert("eph_c", b64(m_ephC));
    obj.insert("cid", b64(cid));
    QJsonArray algs;
    algs.append(QStringLiteral("X25519"));
    algs.append(QStringLiteral("AES-256-GCM"));
    algs.append(QStringLiteral("RSA-PSS-SHA256"));
    algs.append(QStringLiteral("HKDF-SHA256"));
    obj.insert("algs", algs);

    m_status = Handshaking;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

// ------------------------------------------------------------------
// 握手帧分派
// ------------------------------------------------------------------
bool SecureChannel::handleHandshake(const QJsonObject &frame, QByteArray *reply, QString *err)
{
    const QString hs = frame.value("hs").toString();

    bool ok = false;
    if (m_role == Client) {
        if (hs == QLatin1String("SH")) {
            // 已经建立会话后再来一次 SH，只会把密钥和序号搞乱，直接拒绝
            if (m_status == Ready) return fail("握手已完成，拒绝重新握手");
            ok = onServerHello(frame, reply);
        }
        else if (hs == QLatin1String("SF")) {
            if (m_status != Handshaking) return fail("握手状态错乱：不该在此刻收到 SF");
            ok = onServerFinish(frame, reply);
        } else {
            return fail(QString("客户端收到了未知握手报文 %1").arg(hs));
        }
    } else {
        if (hs == QLatin1String("CH")) {
            // 已就绪的连接不允许被一条新 CH 把会话"重置"掉
            if (m_status == Ready) return fail("握手已完成，拒绝重新握手");
            ok = onClientHello(frame, reply);
        }
        else if (hs == QLatin1String("CF")) {
            if (m_status != Handshaking) return fail("握手状态错乱：不该在此刻收到 CF");
            ok = onClientFinish(frame, reply);
        } else {
            return fail(QString("服务端收到了未知握手报文 %1").arg(hs));
        }
    }

    if (!ok && err) *err = m_lastError;
    return ok;
}

// ------------------------------------------------------------------
// 服务端侧实现在 Server/netdb/handshake.cpp；这份文件只保留客户端分支。
// 为了避免两处状态机走偏，非本角色的入口一律判为失败。
// ------------------------------------------------------------------
bool SecureChannel::onClientHello(const QJsonObject &frame, QByteArray *reply)
{
    Q_UNUSED(frame) Q_UNUSED(reply)
    return fail("客户端不接受 CH");
}

bool SecureChannel::onClientFinish(const QJsonObject &frame, QByteArray *reply)
{
    Q_UNUSED(frame) Q_UNUSED(reply)
    return fail("客户端不接受 CF");
}

bool SecureChannel::onServerHello(const QJsonObject &frame, QByteArray *reply)
{
    if (frame.value("ver").toInt() != int(kVer))
        return fail("服务器协议版本不是 2，拒绝降级");

    m_nonceS = unb64(frame.value("nonce_s").toString());
    m_ephS   = unb64(frame.value("eph_s").toString());
    m_sid    = unb64(frame.value("sid").toString());
    if (m_nonceS.size() != 32 || m_ephS.size() != 32 || m_sid.size() != 16)
        return fail("SH 字段长度不正确");

    const QJsonObject cert = frame.value("cert").toObject();
    const QString pubPem = cert.value("pub").toString();
    const QByteArray certFp = unb64(cert.value("fp").toString());
    if (pubPem.trimmed().isEmpty() || certFp.size() != 32)
        return fail("SH 缺少服务器公钥或指纹");

    // ① 证书自洽：fp 必须真的是这份公钥的指纹，防止报一个指纹、给另一把钥匙
    m_serverFp = spkiDigest(pubPem);
    if (m_serverFp.size() != 32)
        return fail("无法解析服务器公钥");
    if (!CryptoUtil::constantTimeEquals(m_serverFp, certFp))
        return fail("服务器公钥与随包指纹不符");

    // ② 与本地固定值比对：自签场景下这是唯一的信任锚
    const bool firstTime = m_pinnedFp.isEmpty();
    if (!firstTime && !CryptoUtil::constantTimeEquals(m_pinnedFp, m_serverFp)) {
        return fail(QString("服务器身份指纹与本地记录不一致（本地 %1，本次 %2）。"
                            "可能是换了服务器，也可能是中间人劫持；"
                            "确认无误后删除 Data/Keys/server.pub 再重试。")
                    .arg(QString::fromLatin1(m_pinnedFp.toHex().left(16)))
                    .arg(QString::fromLatin1(m_serverFp.toHex().left(16))));
    }

    // ③ 验签：nonce 与临时公钥都在签名范围里，中间人改一位就露馅
    m_transcript = buildTranscript(m_serverFp);
    QByteArray sig = unb64(frame.value("sig").toString());
    if (sig.isEmpty() || !CryptoUtil::verifyDetached(pubPem, m_transcript, sig))
        return fail("服务器签名校验失败（SH 被篡改或不是该服务器的私钥所签）");

    if (!deriveSessionKeys())
        return fail(QString("派生会话密钥失败：%1").arg(CryptoUtil::lastError()));

    // ④ CF：证明自己算出了同一把 kc2s
    QJsonObject cf;
    cf.insert("hs", QStringLiteral("CF"));
    cf.insert("fin", b64(CryptoUtil::hmacSha256(m_kc2s, finishInput(m_transcript))));
    *reply = QJsonDocument(cf).toJson(QJsonDocument::Compact);

    if (firstTime) {
        m_learnedPub = pubPem;
        qInfo() << "[握手] 首次连接，固定服务器指纹" << m_serverFp.toHex().left(8);
        emit serverFingerprintLearned(m_serverFp, pubPem);
    }
    return true;
}

bool SecureChannel::onServerFinish(const QJsonObject &frame, QByteArray *reply)
{
    Q_UNUSED(reply)
    if (!checkFinish(frame, m_ks2c))
        return fail("服务器未能证明持有 ks2c（SF 校验失败）");
    becomeReady();
    return true;
}

bool SecureChannel::checkFinish(const QJsonObject &frame, const QByteArray &key)
{
    const QByteArray got = unb64(frame.value("fin").toString());
    if (got.isEmpty() || key.isEmpty()) return false;
    const QByteArray want = CryptoUtil::hmacSha256(key, finishInput(m_transcript));
    return CryptoUtil::constantTimeEquals(got, want);
}

// ------------------------------------------------------------------
// 密钥派生
// ------------------------------------------------------------------
QByteArray SecureChannel::buildTranscript(const QByteArray &fp) const
{
    QByteArray t("MC2/SH", 6);
    t.append(kVer);
    t.append(m_nonceC);     // 32
    t.append(m_nonceS);     // 32
    t.append(m_ephC);       // 32
    t.append(m_ephS);       // 32
    t.append(m_sid);        // 16
    t.append(fp);           // 32
    return CryptoUtil::sha256(t);
}

bool SecureChannel::deriveSessionKeys()
{
    if (m_ephPriv.isEmpty()) return false;

    const QByteArray peer = (m_role == Client) ? m_ephS : m_ephC;
    QByteArray z;
    if (!CryptoUtil::x25519SharedSecret(m_ephPriv, peer, &z)) return false;

    // 临时私钥用完立刻销毁：即使本进程之后被 dump，也拿不到这次会话的钥匙
    m_ephPriv.fill(0);
    m_ephPriv.clear();

    const QByteArray salt = CryptoUtil::sha256(m_nonceC + m_nonceS);
    const QByteArray prk  = CryptoUtil::hkdfExtract(salt, z);
    if (prk.isEmpty()) return false;

    m_kc2s = CryptoUtil::hkdfExpand(prk, QByteArray("MC2/c2s") + m_sid, 32);
    m_ks2c = CryptoUtil::hkdfExpand(prk, QByteArray("MC2/s2c") + m_sid, 32);
    return m_kc2s.size() == 32 && m_ks2c.size() == 32;
}

// ------------------------------------------------------------------
// 数据面
// ------------------------------------------------------------------
QByteArray SecureChannel::seal(const QByteArray &plain, QString *err)
{
    if (m_status != Ready) {
        if (err) *err = "加密通道尚未就绪";
        return QByteArray();
    }

    const bool isClient = (m_role == Client);
    const QByteArray &key = isClient ? m_kc2s : m_ks2c;
    const quint64 seq = m_sendSeq;

    QByteArray ct, tag;
    if (!CryptoUtil::aesGcmSeal(key, recordNonce(seq), recordAad(isClient, seq), plain, &ct, &tag)) {
        if (err) *err = CryptoUtil::lastError();
        return QByteArray();
    }
    ++m_sendSeq;

    QJsonObject obj;
    obj.insert("c", b64(ct + tag));
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

bool SecureChannel::open(const QJsonObject &frame, QByteArray *plain, QString *err)
{
    if (m_status != Ready) {
        if (err) *err = "加密通道尚未就绪";
        return false;
    }
    if (!plain) return false;

    const QByteArray raw = unb64(frame.value("c").toString());
    if (raw.size() < 16) {
        if (err) *err = "密文帧长度不足";
        return false;
    }
    const QByteArray ct  = raw.left(raw.size() - 16);
    const QByteArray tag = raw.right(16);

    const bool fromClient = (m_role == Server);
    const quint64 seq = m_recvSeq;
    const QByteArray &key = fromClient ? m_kc2s : m_ks2c;

    if (!CryptoUtil::aesGcmOpen(key, recordNonce(seq), recordAad(fromClient, seq), ct, tag, plain)) {
        if (err) *err = "链路帧解密失败（序号错位、被篡改或密钥不符）";
        return false;
    }
    ++m_recvSeq;
    return true;
}

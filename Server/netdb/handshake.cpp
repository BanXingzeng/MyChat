#include "handshake.h"

#include "comapi/cryptoutil.h"
#include "comapi/myapp.h"

#include <QDir>
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

/// SHA256(SubjectPublicKeyInfo DER)，客户端用同一函数算出指纹后比对
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

QByteArray recordAad(bool fromClient, quint64 seq)
{
    QByteArray aad("MC2/REC/v2", 10);
    aad.append(static_cast<char>(fromClient ? 1 : 2));
    for (int i = 7; i >= 0; --i)
        aad.append(static_cast<char>((seq >> (i * 8)) & 0xFF));
    return aad;
}

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

QString g_serverPriv;
QByteArray g_serverFp;

} // namespace

// ------------------------------------------------------------------
// 服务器身份密钥
// ------------------------------------------------------------------
namespace ServerIdentity {

QString privateKeyPem()
{
    if (!g_serverPriv.isEmpty()) return g_serverPriv;

    const QString dir = MyApp::m_strDataPath + "Keys/";
    QDir().mkpath(dir);
    const QString path = dir + "server.priv.pem";

    QString pem = CryptoUtil::loadPrivateKey(path);
    if (pem.isEmpty() || !CryptoUtil::isValidPrivateKey(pem)) {
        QString priv, pub;
        if (!CryptoUtil::generateKeyPair(2048, &priv, &pub)) {
            qWarning() << "[握手] 服务器身份密钥生成失败：" << CryptoUtil::lastError();
            return QString();
        }
        CryptoUtil::savePrivateKey(path, priv);
        pem = priv;
        qInfo() << "[握手] 已生成服务器身份密钥：" << path;
    }
    g_serverPriv = pem;
    return g_serverPriv;
}

QString publicKeyPem()
{
    const QString priv = privateKeyPem();
    return priv.isEmpty() ? QString() : CryptoUtil::publicKeyFromPrivate(priv);
}

QByteArray fingerprint()
{
    if (g_serverFp.size() != 32) {
        g_serverFp = spkiDigest(publicKeyPem());
        if (g_serverFp.size() == 32)
            qInfo() << "[握手] 服务器身份指纹" << g_serverFp.toHex().left(8);
    }
    return g_serverFp;
}

} // namespace ServerIdentity

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
    qInfo() << "[握手] 加密通道就绪，会话" << m_sid.toHex().left(8);
    emit handshakeReady();
}

QByteArray SecureChannel::clientHello()
{
    fail("服务端不接受 CH 生成请求");
    return QByteArray();
}

bool SecureChannel::handleHandshake(const QJsonObject &frame, QByteArray *reply, QString *err)
{
    const QString hs = frame.value("hs").toString();

    bool ok = false;
    if (m_role == Server) {
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
    } else {
        if (hs == QLatin1String("SH")) {
            // 已经建立会话后再来一次 SH，只会把密钥和序号搞乱，直接拒绝
            if (m_status == Ready) return fail("握手已完成，拒绝重新握手");
            ok = onServerHello(frame, reply);
        }
        else if (hs == QLatin1String("SF")) ok = onServerFinish(frame, reply);
        else return fail(QString("未知握手报文 %1").arg(hs));
    }

    if (!ok && err) *err = m_lastError;
    return ok;
}

bool SecureChannel::onServerHello(const QJsonObject &frame, QByteArray *reply)
{
    Q_UNUSED(frame) Q_UNUSED(reply)
    return fail("服务端不接受 SH");
}

bool SecureChannel::onServerFinish(const QJsonObject &frame, QByteArray *reply)
{
    Q_UNUSED(frame) Q_UNUSED(reply)
    return fail("服务端不接受 SF");
}

// ------------------------------------------------------------------
// 服务端：收 CH -> 出 SH
// ------------------------------------------------------------------
bool SecureChannel::onClientHello(const QJsonObject &frame, QByteArray *reply)
{
    if (!reply) return fail("内部错误：reply 为空");
    if (frame.value("ver").toInt() != int(kVer))
        return fail("客户端协议版本不是 2，拒绝降级到明文");

    m_nonceC = unb64(frame.value("nonce_c").toString());
    m_ephC   = unb64(frame.value("eph_c").toString());
    if (m_nonceC.size() != 32 || m_ephC.size() != 32)
        return fail("CH 字段长度不正确");

    if (!CryptoUtil::generateX25519KeyPair(&m_ephPriv, &m_ephS)
            || !CryptoUtil::random(32, &m_nonceS)
            || !CryptoUtil::random(16, &m_sid)) {
        return fail(QString("生成握手材料失败：%1").arg(CryptoUtil::lastError()));
    }

    const QString pub  = ServerIdentity::publicKeyPem();
    const QString priv = ServerIdentity::privateKeyPem();
    if (pub.isEmpty() || priv.isEmpty())
        return fail("服务器身份密钥不可用，不能建立可信链路");

    m_serverFp = spkiDigest(pub);
    if (m_serverFp.size() != 32)
        return fail("无法计算服务器公钥指纹");

    m_transcript = buildTranscript(m_serverFp);

    QByteArray sig;
    if (!CryptoUtil::signDetached(priv, m_transcript, &sig))
        return fail(QString("服务器签名失败：%1").arg(CryptoUtil::lastError()));

    QJsonObject cert;
    cert.insert("uid", 0);
    cert.insert("pub", pub);
    cert.insert("fp", b64(m_serverFp));

    QJsonObject sh;
    sh.insert("hs", QStringLiteral("SH"));
    sh.insert("ver", int(kVer));
    sh.insert("nonce_s", b64(m_nonceS));
    sh.insert("eph_s", b64(m_ephS));
    sh.insert("sid", b64(m_sid));
    sh.insert("cert", cert);
    sh.insert("sig", b64(sig));
    *reply = QJsonDocument(sh).toJson(QJsonDocument::Compact);

    if (!deriveSessionKeys())
        return fail(QString("派生会话密钥失败：%1").arg(CryptoUtil::lastError()));

    m_status = Handshaking;
    return true;
}

// ------------------------------------------------------------------
// 服务端：收 CF -> 出 SF
// ------------------------------------------------------------------
bool SecureChannel::onClientFinish(const QJsonObject &frame, QByteArray *reply)
{
    if (!checkFinish(frame, m_kc2s))
        return fail("客户端未能证明持有 kc2s（口令或握手被中间人改过）");

    QJsonObject sf;
    sf.insert("hs", QStringLiteral("SF"));
    sf.insert("fin", b64(CryptoUtil::hmacSha256(m_ks2c, finishInput(m_transcript))));
    *reply = QJsonDocument(sf).toJson(QJsonDocument::Compact);

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

QByteArray SecureChannel::buildTranscript(const QByteArray &fp) const
{
    QByteArray t("MC2/SH", 6);
    t.append(kVer);
    t.append(m_nonceC);
    t.append(m_nonceS);
    t.append(m_ephC);
    t.append(m_ephS);
    t.append(m_sid);
    t.append(fp);
    return CryptoUtil::sha256(t);
}

bool SecureChannel::deriveSessionKeys()
{
    if (m_ephPriv.isEmpty()) return false;

    const QByteArray peer = (m_role == Client) ? m_ephS : m_ephC;
    QByteArray z;
    if (!CryptoUtil::x25519SharedSecret(m_ephPriv, peer, &z)) return false;

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

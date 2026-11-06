#include "cryptoutil.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/opensslv.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QDebug>

// AES-256-GCM 参数
static const int kKeyLen = 32;
static const int kIvLen  = 12;
static const int kTagLen = 16;
static const int kRsaBits = 2048;

static QString g_lastError;

// ------------------------------------------------------------------
// 内部工具
// ------------------------------------------------------------------
static QString collectErrors(const QString &prefix)
{
    QStringList list;
    unsigned long code = 0;
    char buf[256];
    while ((code = ERR_get_error()) != 0) {
        ERR_error_string_n(code, buf, sizeof(buf));
        list.append(QString::fromLatin1(buf));
    }
    return list.isEmpty() ? prefix : prefix + ": " + list.join("; ");
}

static void setError(const QString &msg)
{
    g_lastError = msg;
    qWarning() << "[Crypto] " << msg;
}

static QByteArray randomBytes(int len)
{
    QByteArray out(len, 0);
    if (len <= 0) return out;
    if (RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), len) != 1) {
        setError(collectErrors("生成随机数失败"));
        return QByteArray();
    }
    return out;
}

static EVP_PKEY *pemToPublicKey(const QString &pem)
{
    if (pem.trimmed().isEmpty()) return nullptr;
    const QByteArray raw = pem.toUtf8();
    BIO *bio = BIO_new_mem_buf(raw.constData(), raw.size());
    if (!bio) return nullptr;
    EVP_PKEY *key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!key) {
        // 兼容直接给私钥 PEM 的情况：从私钥里取公钥部分
        BIO *bio2 = BIO_new_mem_buf(raw.constData(), raw.size());
        if (bio2) {
            EVP_PKEY *priv = PEM_read_bio_PrivateKey(bio2, nullptr, nullptr, nullptr);
            BIO_free(bio2);
            if (priv) return priv;   // 私钥对象同样可用于公钥加密
        }
        setError(collectErrors("解析接收方公钥失败"));
    }
    return key;
}

static EVP_PKEY *pemToPrivateKey(const QString &pem)
{
    if (pem.trimmed().isEmpty()) return nullptr;
    const QByteArray raw = pem.toUtf8();
    BIO *bio = BIO_new_mem_buf(raw.constData(), raw.size());
    if (!bio) return nullptr;
    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!key) setError(collectErrors("解析本机私钥失败"));
    return key;
}

// ------------------------------------------------------------------
// 版本 / 错误
// ------------------------------------------------------------------
QString CryptoUtil::version()
{
    return QString::fromLatin1(OpenSSL_version(OPENSSL_VERSION));
}

QString CryptoUtil::lastError()
{
    return g_lastError;
}

void CryptoUtil::clearError()
{
    g_lastError.clear();
    ERR_clear_error();
}

// ------------------------------------------------------------------
// 密钥对
// ------------------------------------------------------------------
bool CryptoUtil::generateKeyPair(int bits, QString *privPem, QString *pubPem)
{
    clearError();
    if (bits < 1024) bits = kRsaBits;

    bool ok = false;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY *pkey = nullptr;
    BIO *privBio = nullptr;
    BIO *pubBio = nullptr;

    do {
        if (!ctx) { setError(collectErrors("创建密钥上下文失败")); break; }
        if (EVP_PKEY_keygen_init(ctx) <= 0) { setError(collectErrors("初始化密钥生成失败")); break; }
        if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0) { setError(collectErrors("设置密钥长度失败")); break; }
        if (EVP_PKEY_keygen(ctx, &pkey) <= 0) { setError(collectErrors("生成 RSA 密钥失败")); break; }

        privBio = BIO_new(BIO_s_mem());
        pubBio  = BIO_new(BIO_s_mem());
        if (!privBio || !pubBio) { setError("创建内存 BIO 失败"); break; }
        if (PEM_write_bio_PrivateKey(privBio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            setError(collectErrors("导出私钥失败")); break;
        }
        if (PEM_write_bio_PUBKEY(pubBio, pkey) != 1) {
            setError(collectErrors("导出公钥失败")); break;
        }

        {
            char *data = nullptr;
            long len = BIO_get_mem_data(privBio, &data);
            if (privPem) *privPem = QString::fromLatin1(data, int(len));
            len = BIO_get_mem_data(pubBio, &data);
            if (pubPem) *pubPem = QString::fromLatin1(data, int(len));
        }
        ok = true;
    } while (false);

    if (ctx) EVP_PKEY_CTX_free(ctx);
    if (pkey) EVP_PKEY_free(pkey);
    if (privBio) BIO_free(privBio);
    if (pubBio) BIO_free(pubBio);
    return ok;
}

bool CryptoUtil::isValidPrivateKey(const QString &privPem)
{
    if (privPem.trimmed().isEmpty()) return false;
    EVP_PKEY *key = pemToPrivateKey(privPem);
    const bool ok = (key != nullptr);
    if (key) EVP_PKEY_free(key);
    return ok;
}

QString CryptoUtil::publicKeyFromPrivate(const QString &privPem)
{
    clearError();
    EVP_PKEY *pkey = pemToPrivateKey(privPem);
    if (!pkey) return QString();

    QString result;
    BIO *bio = BIO_new(BIO_s_mem());
    if (bio && PEM_write_bio_PUBKEY(bio, pkey) == 1) {
        char *data = nullptr;
        const long len = BIO_get_mem_data(bio, &data);
        result = QString::fromLatin1(data, int(len));
    }
    if (bio) BIO_free(bio);
    EVP_PKEY_free(pkey);
    return result;
}

// ------------------------------------------------------------------
// 密钥文件
// ------------------------------------------------------------------
static QString safeFileName(const QString &name)
{
    QString out = name;
    const QString bad = "\\/:*?\"<>| ";
    for (int i = 0; i < bad.size(); ++i)
        out.replace(bad.at(i), '_');
    return out;
}

bool CryptoUtil::savePrivateKey(const QString &filePath, const QString &privPem)
{
    QFileInfo info(filePath);
    QDir().mkpath(info.absolutePath());

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(QString("保存私钥失败：%1").arg(file.errorString()));
        return false;
    }
    file.write(privPem.toLatin1());
    file.close();
    // 私钥是敏感材料，落盘后立即收紧为仅属主可读写。
    // 不设的话在 Linux 上会跟着 umask 变成 644，同机其他用户可读。
    QFile::setPermissions(filePath, QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

QString CryptoUtil::loadPrivateKey(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    const QString pem = QString::fromLatin1(file.readAll());
    file.close();
    return pem;
}

QString CryptoUtil::ensureKeyPair(const QString &dir, const QString &userName, QString *pubPem)
{
    clearError();
    const QString filePath = QDir(dir).filePath(safeFileName(userName) + ".priv.pem");

    QString privPem = loadPrivateKey(filePath);
    if (!privPem.isEmpty() && isValidPrivateKey(privPem)) {
        // 老版本生成的文件可能是 644，读到就顺手收紧一次
        QFile::setPermissions(filePath, QFile::ReadOwner | QFile::WriteOwner);
        if (pubPem) *pubPem = publicKeyFromPrivate(privPem);
        return privPem;
    }

    // 生成新密钥对
    QString newPriv, newPub;
    if (!generateKeyPair(kRsaBits, &newPriv, &newPub)) return QString();
    if (!savePrivateKey(filePath, newPriv)) return QString();
    if (pubPem) *pubPem = newPub;
    qDebug() << "[Crypto] 已生成新密钥对：" << filePath;
    return newPriv;
}

// ------------------------------------------------------------------
// RSA-OAEP 包裹 / 解包
// ------------------------------------------------------------------
bool CryptoUtil::wrap(const QString &pubPem, const QByteArray &data, QByteArray *out)
{
    clearError();
    if (!out) return false;
    EVP_PKEY *pkey = pemToPublicKey(pubPem);
    if (!pkey) return false;

    bool ok = false;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, nullptr);
    do {
        if (!ctx) { setError(collectErrors("创建加密上下文失败")); break; }
        if (EVP_PKEY_encrypt_init(ctx) <= 0) { setError(collectErrors("初始化公钥加密失败")); break; }
        if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) { setError(collectErrors("设置 OAEP 填充失败")); break; }
        if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) <= 0) { setError(collectErrors("设置 OAEP 摘要失败")); break; }
        if (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) <= 0) { setError(collectErrors("设置 MGF1 摘要失败")); break; }

        size_t len = 0;
        if (EVP_PKEY_encrypt(ctx, nullptr, &len,
                             reinterpret_cast<const unsigned char *>(data.constData()),
                             size_t(data.size())) <= 0) {
            setError(collectErrors("计算密文长度失败")); break;
        }
        out->resize(int(len));
        if (EVP_PKEY_encrypt(ctx, reinterpret_cast<unsigned char *>(out->data()), &len,
                             reinterpret_cast<const unsigned char *>(data.constData()),
                             size_t(data.size())) <= 0) {
            out->clear();
            setError(collectErrors("公钥加密失败")); break;
        }
        out->resize(int(len));
        ok = true;
    } while (false);

    if (ctx) EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

bool CryptoUtil::unwrap(const QString &privPem, const QByteArray &data, QByteArray *out)
{
    clearError();
    if (!out) return false;
    EVP_PKEY *pkey = pemToPrivateKey(privPem);
    if (!pkey) return false;

    bool ok = false;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, nullptr);
    do {
        if (!ctx) { setError(collectErrors("创建解密上下文失败")); break; }
        if (EVP_PKEY_decrypt_init(ctx) <= 0) { setError(collectErrors("初始化解密失败")); break; }
        if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) { setError(collectErrors("设置 OAEP 填充失败")); break; }
        if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) <= 0) { setError(collectErrors("设置 OAEP 摘要失败")); break; }
        if (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) <= 0) { setError(collectErrors("设置 MGF1 摘要失败")); break; }

        // 注意：OpenSSL 要求输出缓冲区不小于模长（2048 位 = 256 字节）
        out->resize(int(EVP_PKEY_get_size(pkey)) + 16);
        size_t len = size_t(out->size());
        if (EVP_PKEY_decrypt(ctx, reinterpret_cast<unsigned char *>(out->data()), &len,
                             reinterpret_cast<const unsigned char *>(data.constData()),
                             size_t(data.size())) <= 0) {
            out->clear();
            setError(collectErrors("私钥解密失败")); break;
        }
        out->resize(int(len));
        ok = true;
    } while (false);

    if (ctx) EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

// ------------------------------------------------------------------
// 消息包
// ------------------------------------------------------------------
bool CryptoUtil::isPacket(const QString &text)
{
    const QString t = text.trimmed();
    if (!t.startsWith('{')) return false;

    const QJsonObject obj = QJsonDocument::fromJson(t.toUtf8()).object();
    if (obj.isEmpty()) return false;

    // 按密文共同特征判定，兼容三种信封：v1（有 alg）/ v2（eks 为对象）/ v3（eks 为数组、无 alg）
    if (!obj.contains("ct") || !obj.contains("tag") || !obj.contains("iv")) return false;
    const QJsonValue eks = obj.value("eks");
    return eks.isArray() || eks.isObject();
}

QString CryptoUtil::encryptMessage(const QString &plain, const QMap<int, QString> &recipients)
{
    clearError();
    if (recipients.isEmpty()) {
        setError("没有可用的接收方公钥");
        return QString();
    }

    const QByteArray key = randomBytes(kKeyLen);
    const QByteArray iv  = randomBytes(kIvLen);
    if (key.isEmpty() || iv.isEmpty()) return QString();

    const QByteArray src = plain.toUtf8();
    QByteArray ct(src.size() + kTagLen + 8, 0);
    QByteArray tag(kTagLen, 0);

    bool ok = false;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    do {
        if (!ctx) { setError(collectErrors("创建 AES 上下文失败")); break; }
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) { setError(collectErrors("初始化 AES-GCM 失败")); break; }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvLen, nullptr) != 1) { setError(collectErrors("设置 IV 长度失败")); break; }
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                               reinterpret_cast<const unsigned char *>(key.constData()),
                               reinterpret_cast<const unsigned char *>(iv.constData())) != 1) {
            setError(collectErrors("载入 AES 密钥失败")); break;
        }

        int len = 0, total = 0;
        if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(ct.data()), &len,
                              reinterpret_cast<const unsigned char *>(src.constData()), src.size()) != 1) {
            setError(collectErrors("AES 加密失败")); break;
        }
        total = len;
        if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(ct.data()) + total, &len) != 1) {
            setError(collectErrors("AES 加密收尾失败")); break;
        }
        total += len;
        ct.resize(total);
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, tag.data()) != 1) {
            setError(collectErrors("获取认证标签失败")); break;
        }
        ok = true;
    } while (false);
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    if (!ok) return QString();

    // 为每个接收人包裹 AES 密钥
    QJsonObject eks;
    for (QMap<int, QString>::const_iterator it = recipients.constBegin(); it != recipients.constEnd(); ++it) {
        QByteArray wrapped;
        if (!wrap(it.value(), key, &wrapped)) {
            setError(QString("为用户 %1 包裹密钥失败：%2").arg(it.key()).arg(lastError()));
            return QString();
        }
        eks.insert(QString::number(it.key()), QString::fromLatin1(wrapped.toBase64()));
    }

    QJsonObject packet;
    packet.insert("v", 1);
    packet.insert("alg", "AES-256-GCM");
    packet.insert("iv", QString::fromLatin1(iv.toBase64()));
    packet.insert("ct", QString::fromLatin1(ct.toBase64()));
    packet.insert("tag", QString::fromLatin1(tag.toBase64()));
    packet.insert("eks", eks);

    return QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact));
}

bool CryptoUtil::decryptMessage(const QString &packet, const QString &privPem, int myId,
                                QString *plain, QString *err)
{
    clearError();
    const QString fail = QStringLiteral("解密失败");

    const QJsonDocument doc = QJsonDocument::fromJson(packet.toUtf8());
    if (!doc.isObject()) {
        if (err) *err = "不是有效的密文包";
        return false;
    }
    const QJsonObject obj = doc.object();
    if (obj.value("alg").toString() != "AES-256-GCM") {
        if (err) *err = "不支持的加密算法";
        return false;
    }

    const QJsonObject eks = obj.value("eks").toObject();
    const QString myKey = eks.value(QString::number(myId)).toString();
    if (myKey.isEmpty()) {
        if (err) *err = QString("密文中没有发给本用户（%1）的密钥").arg(myId);
        return false;
    }

    const QByteArray iv  = QByteArray::fromBase64(obj.value("iv").toString().toLatin1());
    const QByteArray ct  = QByteArray::fromBase64(obj.value("ct").toString().toLatin1());
    const QByteArray tag = QByteArray::fromBase64(obj.value("tag").toString().toLatin1());
    const QByteArray wrapped = QByteArray::fromBase64(myKey.toLatin1());

    QByteArray aesKey;
    if (!unwrap(privPem, wrapped, &aesKey)) {
        if (err) *err = QString("解开本机会话密钥失败：%1").arg(lastError());
        return false;
    }
    if (aesKey.size() != kKeyLen) {
        if (err) *err = "会话密钥长度异常";
        return false;
    }

    QByteArray out(ct.size() + kTagLen + 8, 0);
    bool ok = false;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    do {
        if (!ctx) { setError(collectErrors("创建 AES 上下文失败")); break; }
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) { setError(collectErrors("初始化 AES-GCM 失败")); break; }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr) != 1) { setError(collectErrors("设置 IV 长度失败")); break; }
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                               reinterpret_cast<const unsigned char *>(aesKey.constData()),
                               reinterpret_cast<const unsigned char *>(iv.constData())) != 1) {
            setError(collectErrors("载入 AES 密钥失败")); break;
        }

        int len = 0, total = 0;
        if (EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &len,
                              reinterpret_cast<const unsigned char *>(ct.constData()), ct.size()) != 1) {
            setError(collectErrors("AES 解密失败")); break;
        }
        total = len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(),
                                const_cast<char *>(tag.constData())) != 1) {
            setError(collectErrors("设置认证标签失败")); break;
        }
        if (EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total, &len) <= 0) {
            setError("消息认证失败（密文可能被篡改）"); break;
        }
        total += len;
        out.resize(total);
        ok = true;
    } while (false);
    if (ctx) EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        if (err) *err = lastError();
        return false;
    }
    if (plain) *plain = QString::fromUtf8(out);
    Q_UNUSED(fail)
    return true;
}

QList<int> CryptoUtil::packetRecipients(const QString &packet)
{
    QList<int> ids;
    const QJsonDocument doc = QJsonDocument::fromJson(packet.toUtf8());
    if (!doc.isObject()) return ids;
    const QJsonObject eks = doc.object().value("eks").toObject();
    for (QJsonObject::const_iterator it = eks.constBegin(); it != eks.constEnd(); ++it)
        ids.append(it.key().toInt());
    return ids;
}

// ==================================================================
// 签名 + v2 消息信封
//
// 与 v1 的区别只有两点，但这两点决定了"能不能证明是谁发的"：
//  1. 头部字段算进 AAD，任何一位被改，GCM 就解不开 -> 防篡改
//  2. 用发送者私钥对 (AAD + 密文摘要) 签名 -> 防伪造
// ==================================================================
static void appendU64(QByteArray &buf, quint64 v)
{
    for (int i = 7; i >= 0; --i)
        buf.append(static_cast<char>((v >> (i * 8)) & 0xFF));
}

static void appendU32(QByteArray &buf, quint32 v)
{
    for (int i = 3; i >= 0; --i)
        buf.append(static_cast<char>((v >> (i * 8)) & 0xFF));
}

/// 大端读 32 位，与 appendU32 对应
static quint32 readU32(const char *p)
{
    return (static_cast<quint32>(static_cast<quint8>(p[0])) << 24)
         | (static_cast<quint32>(static_cast<quint8>(p[1])) << 16)
         | (static_cast<quint32>(static_cast<quint8>(p[2])) << 8)
         |  static_cast<quint32>(static_cast<quint8>(p[3]));
}

/// eks 的规范化字节串：只拼包裹密钥本身，**不带 uid**。
/// 带 uid 就等于把收件人名单写回报文，前面把 from/to 加密就白做了。
/// 顺序固定（发送端按 uid 升序排好），签名两端都能重算。
static QByteArray eksCanonical(const QList<QByteArray> &wrapped)
{
    QByteArray buf;
    for (const QByteArray &w : wrapped) {
        appendU32(buf, static_cast<quint32>(w.size()));
        buf.append(w);
    }
    return buf;
}

QByteArray CryptoUtil::sha256(const QByteArray &data)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(data.constData(), size_t(data.size()), md, &len, EVP_sha256(), nullptr) != 1)
        return QByteArray();
    return QByteArray(reinterpret_cast<const char *>(md), int(len));
}

bool CryptoUtil::signDetached(const QString &privPem, const QByteArray &data, QByteArray *sig)
{
    clearError();
    if (!sig) return false;
    EVP_PKEY *pkey = pemToPrivateKey(privPem);
    if (!pkey) return false;

    bool ok = false;
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    EVP_PKEY_CTX *pctx = nullptr;
    do {
        if (!md) { setError("创建签名上下文失败"); break; }
        if (EVP_DigestSignInit(md, &pctx, EVP_sha256(), nullptr, pkey) != 1) {
            setError(collectErrors("初始化签名失败")); break;
        }
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0) { setError("设置 PSS 填充失败"); break; }
        if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) <= 0) { setError("设置盐长度失败"); break; }

        size_t len = 0;
        if (EVP_DigestSign(md, nullptr, &len,
                           reinterpret_cast<const unsigned char *>(data.constData()),
                           size_t(data.size())) != 1) {
            setError(collectErrors("计算签名长度失败")); break;
        }
        sig->resize(int(len));
        if (EVP_DigestSign(md, reinterpret_cast<unsigned char *>(sig->data()), &len,
                           reinterpret_cast<const unsigned char *>(data.constData()),
                           size_t(data.size())) != 1) {
            sig->clear();
            setError(collectErrors("签名失败")); break;
        }
        sig->resize(int(len));
        ok = true;
    } while (false);

    if (md) EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    return ok;
}

bool CryptoUtil::verifyDetached(const QString &pubPem, const QByteArray &data, const QByteArray &sig)
{
    clearError();
    if (pubPem.trimmed().isEmpty() || sig.isEmpty()) return false;
    EVP_PKEY *pkey = pemToPublicKey(pubPem);
    if (!pkey) return false;

    bool ok = false;
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    EVP_PKEY_CTX *pctx = nullptr;
    do {
        if (!md) break;
        if (EVP_DigestVerifyInit(md, &pctx, EVP_sha256(), nullptr, pkey) != 1) break;
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0) break;
        if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) <= 0) break;
        ok = (EVP_DigestVerify(md,
                               reinterpret_cast<const unsigned char *>(sig.constData()),
                               size_t(sig.size()),
                               reinterpret_cast<const unsigned char *>(data.constData()),
                               size_t(data.size())) == 1);
    } while (false);

    if (md) EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    return ok;
}

QByteArray CryptoUtil::buildAad(const MsgHeader &hdr)
{
    // v3：AAD 只绑定「信封外层明文可见」的字段。
    // from/to 已经移进密文，不再写进 AAD —— 它们由 GCM 直接保护，
    // 放进 AAD 反而等于把身份又抄回明文头，白改。
    QByteArray aad("MC2/MSG/v3", 10);
    aad.append('\0');
    aad.append(hdr.scope == QLatin1String("group") ? char(1) : char(0));
    appendU32(aad, static_cast<quint32>(hdr.kind));
    appendU64(aad, static_cast<quint64>(hdr.ts));
    appendU64(aad, hdr.seq);
    aad.append(hdr.mid.left(16));
    return aad;
}

QByteArray CryptoUtil::buildDirProof(int uid, const QString &pubPem)
{
    QByteArray buf("MC2/DIR/v2", 10);
    buf.append('\0');
    appendU64(buf, static_cast<quint64>(uid));
    buf.append(sha256(pubPem.trimmed().toUtf8()));
    return buf;
}

/// 带 AAD 的 AES-256-GCM 加密
static bool gcmSeal(const QByteArray &key, const QByteArray &iv, const QByteArray &aad,
                    const QByteArray &src, QByteArray *ct, QByteArray *tag)
{
    bool ok = false;
    QByteArray out(src.size() + 16, 0);
    tag->resize(kTagLen);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    do {
        if (!ctx) break;
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr) != 1) break;
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                               reinterpret_cast<const unsigned char *>(key.constData()),
                               reinterpret_cast<const unsigned char *>(iv.constData())) != 1) break;
        int len = 0;
        if (!aad.isEmpty()) {
            if (EVP_EncryptUpdate(ctx, nullptr, &len,
                                  reinterpret_cast<const unsigned char *>(aad.constData()),
                                  aad.size()) != 1) break;
        }
        int total = 0;
        if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &len,
                              reinterpret_cast<const unsigned char *>(src.constData()),
                              src.size()) != 1) break;
        total = len;
        if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total, &len) != 1) break;
        total += len;
        out.resize(total);
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, tag->data()) != 1) break;
        *ct = out;
        ok = true;
    } while (false);
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    return ok;
}

/// 带 AAD 的 AES-256-GCM 解密（tag 不符即失败）
static bool gcmOpen(const QByteArray &key, const QByteArray &iv, const QByteArray &aad,
                    const QByteArray &ct, const QByteArray &tag, QByteArray *plain)
{
    bool ok = false;
    QByteArray out(ct.size() + 16, 0);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    do {
        if (!ctx) break;
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr) != 1) break;
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                               reinterpret_cast<const unsigned char *>(key.constData()),
                               reinterpret_cast<const unsigned char *>(iv.constData())) != 1) break;
        int len = 0;
        if (!aad.isEmpty()) {
            if (EVP_DecryptUpdate(ctx, nullptr, &len,
                                  reinterpret_cast<const unsigned char *>(aad.constData()),
                                  aad.size()) != 1) break;
        }
        int total = 0;
        if (EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &len,
                              reinterpret_cast<const unsigned char *>(ct.constData()),
                              ct.size()) != 1) break;
        total = len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(),
                                const_cast<char *>(tag.constData())) != 1) break;
        if (EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total, &len) <= 0) break;
        total += len;
        out.resize(total);
        *plain = out;
        ok = true;
    } while (false);
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    return ok;
}

QString CryptoUtil::encryptEnvelope(const QByteArray &plain, const QMap<int, QString> &recipients,
                                    const MsgHeader &hdr, const QString &senderPrivPem, QString *err)
{
    clearError();
    if (recipients.isEmpty()) {
        if (err) *err = "没有可用的接收方公钥";
        return QString();
    }
    if (hdr.mid.size() != 16) {
        if (err) *err = "消息标识长度必须为 16 字节";
        return QString();
    }
    if (senderPrivPem.trimmed().isEmpty()) {
        if (err) *err = "本机私钥为空，无法签名";
        return QString();
    }

    const QByteArray key = randomBytes(kKeyLen);
    const QByteArray iv  = randomBytes(kIvLen);
    if (key.size() != kKeyLen || iv.size() != kIvLen) {
        if (err) *err = "生成会话密钥失败";
        return QString();
    }

    // 身份进密文：发送者/接收者的 uid 拼在正文前面一起加密，
    // 服务器解开链路后看到的只是一串密文，报文里不再有任何 uid。
    QByteArray inner;
    appendU32(inner, static_cast<quint32>(hdr.from));
    appendU32(inner, static_cast<quint32>(hdr.to));
    inner.append(plain);

    const QByteArray aad = buildAad(hdr);
    QByteArray ct, tag;
    if (!gcmSeal(key, iv, aad, inner, &ct, &tag)) {
        if (err) *err = "AES-GCM 加密失败";
        return QString();
    }

    // 为每个接收人包裹 AES 密钥。
    // 用数组而不是 {"uid": 包裹}：JSON 的键就是 uid，等于把收件人名单明文写进报文。
    // 数组不标归属，接收方用自己私钥逐个试解，解得开的那份就是自己的。
    QList<QByteArray> wrapped;
    QJsonArray eks;
    for (QMap<int, QString>::const_iterator it = recipients.constBegin();
         it != recipients.constEnd(); ++it) {
        QByteArray w;
        if (!wrap(it.value(), key, &w)) {
            if (err) *err = QString("为用户 %1 包裹密钥失败：%2").arg(it.key()).arg(lastError());
            return QString();
        }
        wrapped.append(w);
        eks.append(QString::fromLatin1(w.toBase64()));
    }

    // 签名输入 = 常量头 + AAD + 密文摘要 + 收件人摘要
    QByteArray sigIn("MC2/SIG/v3", 10);
    sigIn.append('\0');
    sigIn.append(aad);
    sigIn.append(sha256(ct));
    sigIn.append(sha256(eksCanonical(wrapped)));

    QByteArray sig;
    if (!signDetached(senderPrivPem, sigIn, &sig)) {
        if (err) *err = QString("消息签名失败：%1").arg(lastError());
        return QString();
    }

    // 外层头只留路由/去重需要的非身份字段
    QJsonObject h;
    h.insert("mid",   QString::fromLatin1(hdr.mid.toBase64()));
    h.insert("scope", hdr.scope);
    h.insert("kind",  hdr.kind);
    h.insert("ts",    static_cast<double>(hdr.ts));
    h.insert("seq",   static_cast<double>(hdr.seq));
    h.insert("key",   hdr.key);

    QJsonObject packet;
    packet.insert("v", 3);
    packet.insert("hdr", h);
    packet.insert("iv",  QString::fromLatin1(iv.toBase64()));
    packet.insert("ct",  QString::fromLatin1(ct.toBase64()));
    packet.insert("tag", QString::fromLatin1(tag.toBase64()));
    packet.insert("eks", eks);
    packet.insert("sig", QString::fromLatin1(sig.toBase64()));

    return QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact));
}

bool CryptoUtil::peekEnvelope(const QString &packet, MsgHeader *hdr, QString *err)
{
    if (!hdr) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(packet.toUtf8());
    if (!doc.isObject()) {
        if (err) *err = "不是有效的密文包";
        return false;
    }
    const QJsonObject obj = doc.object();
    if (obj.value("v").toInt() != 3) {
        if (err) *err = "不是 v3 信封（旧格式没有把身份移入密文）";
        return false;
    }
    const QJsonObject h = obj.value("hdr").toObject();
    hdr->mid   = QByteArray::fromBase64(h.value("mid").toString().toLatin1());
    // from/to 在密文里，peek 阶段拿不到，只能留 0；
    // 需要身份请用 openEnvelope，它会解出真实值。
    hdr->from  = 0;
    hdr->to    = 0;
    hdr->scope = h.value("scope").toString();
    hdr->kind  = h.value("kind").toInt();
    hdr->ts    = static_cast<qint64>(h.value("ts").toDouble());
    hdr->seq   = static_cast<quint64>(h.value("seq").toDouble());
    hdr->key   = h.value("key").toString();

    if (hdr->mid.size() != 16) {
        if (err) *err = "信封头字段不完整";
        return false;
    }
    return true;
}

bool CryptoUtil::openEnvelope(const QString &packet, MsgHeader *hdr, int myId,
                              const QString &myPrivPem, QByteArray *plain,
                              QByteArray *sigInput, QByteArray *sigOut, QString *err)
{
    clearError();
    if (!hdr) {
        if (err) *err = "内部错误：hdr 为空";
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(packet.toUtf8());
    if (!doc.isObject()) {
        if (err) *err = "不是有效的密文包";
        return false;
    }
    const QJsonObject obj = doc.object();
    if (obj.value("v").toInt() != 3) {
        if (err) *err = "不是 v3 信封";
        return false;
    }

    const QJsonObject h = obj.value("hdr").toObject();
    MsgHeader outer;
    outer.mid   = QByteArray::fromBase64(h.value("mid").toString().toLatin1());
    outer.scope = h.value("scope").toString();
    outer.kind  = h.value("kind").toInt();
    outer.ts    = static_cast<qint64>(h.value("ts").toDouble());
    outer.seq   = static_cast<quint64>(h.value("seq").toDouble());
    outer.key   = h.value("key").toString();
    if (outer.mid.size() != 16) {
        if (err) *err = "信封头字段不完整";
        return false;
    }

    const QJsonArray eks = obj.value("eks").toArray();
    if (eks.isEmpty()) {
        if (err) *err = "密文里没有密钥包裹";
        return false;
    }

    // 不标 uid 了：逐个用自己的私钥试解，解得开的那份就是发给本机的。
    // RSA 解不开会很慢？群消息通常几十人，最坏 O(n) 次私钥运算，实际可忽略。
    QList<QByteArray> wrapped;
    QByteArray aesKey;
    for (const QJsonValue &v : eks) {
        const QByteArray w = QByteArray::fromBase64(v.toString().toLatin1());
        if (w.isEmpty()) continue;
        wrapped.append(w);
        if (!aesKey.isEmpty()) continue;      // 已经解出来了，剩下的只用于重算签名
        QByteArray k;
        if (unwrap(myPrivPem, w, &k) && k.size() == kKeyLen) aesKey = k;
    }
    if (aesKey.isEmpty()) {
        if (err) *err = QString("密文里没有发给本用户（%1）的密钥").arg(myId);
        return false;
    }

    const QByteArray iv  = QByteArray::fromBase64(obj.value("iv").toString().toLatin1());
    const QByteArray ct  = QByteArray::fromBase64(obj.value("ct").toString().toLatin1());
    const QByteArray tag = QByteArray::fromBase64(obj.value("tag").toString().toLatin1());
    const QByteArray aad = buildAad(outer);

    QByteArray out;
    if (!gcmOpen(aesKey, iv, aad, ct, tag, &out)) {
        if (err) *err = "消息认证失败：包头或密文被篡改";
        return false;
    }
    if (out.size() < 8) {
        if (err) *err = "密文内容过短";
        return false;
    }

    const int from = static_cast<int>(readU32(out.constData()));
    const int to   = static_cast<int>(readU32(out.constData() + 4));
    if (from <= 0 || to <= 0) {
        if (err) *err = "密文内的收发方标识无效";
        return false;
    }
    // 单聊必须确实是发给本机的；群消息的 to 是群号，不做此检查
    if (outer.scope != QLatin1String("group") && to != myId) {
        if (err) *err = QString("这条消息是发给 %1 的，不是本机（%2）").arg(to).arg(myId);
        return false;
    }

    outer.from = from;
    outer.to   = to;
    *hdr = outer;

    if (plain) *plain = out.mid(8);

    QByteArray sigIn("MC2/SIG/v3", 10);
    sigIn.append('\0');
    sigIn.append(aad);
    sigIn.append(sha256(ct));
    sigIn.append(sha256(eksCanonical(wrapped)));

    if (sigInput) *sigInput = sigIn;
    if (sigOut)   *sigOut = QByteArray::fromBase64(obj.value("sig").toString().toLatin1());
    return true;
}


// ==================================================================
// AES-256-CTR 流式加解密（文件内容）
// ==================================================================
CryptoStream::CryptoStream() : m_ctx(nullptr), m_ok(false)
{
}

CryptoStream::~CryptoStream()
{
    reset();
}

void CryptoStream::reset()
{
    if (m_ctx) {
        EVP_CIPHER_CTX_free(static_cast<EVP_CIPHER_CTX *>(m_ctx));
        m_ctx = nullptr;
    }
    m_ok = false;
}

bool CryptoStream::isActive() const
{
    return m_ok && m_ctx != nullptr;
}

bool CryptoStream::initEncrypt(const QByteArray &key, const QByteArray &iv)
{
    // CTR 模式下加密与解密是同一次运算，统一走 initDecrypt
    return initDecrypt(key, iv);
}

bool CryptoStream::initDecrypt(const QByteArray &key, const QByteArray &iv)
{
    reset();
    if (key.size() != kKeyLen) { setError("文件会话密钥长度不正确（需要 32 字节）"); return false; }
    if (iv.size() != 16)       { setError("文件会话 IV 长度不正确（需要 16 字节）"); return false; }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { setError(collectErrors("创建文件加密上下文失败")); return false; }

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr,
                           reinterpret_cast<const unsigned char *>(key.constData()),
                           reinterpret_cast<const unsigned char *>(iv.constData())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        setError(collectErrors("初始化 AES-256-CTR 失败"));
        return false;
    }

    m_ctx = ctx;
    m_ok = true;
    return true;
}

QByteArray CryptoStream::update(const QByteArray &in)
{
    if (!isActive() || in.isEmpty()) return QByteArray();

    // CTR 模式输出长度不会超过输入长度，多留一点余量防止溢出
    QByteArray out(in.size() + kKeyLen, '\0');
    int len = 0;
    if (EVP_DecryptUpdate(static_cast<EVP_CIPHER_CTX *>(m_ctx),
                          reinterpret_cast<unsigned char *>(out.data()), &len,
                          reinterpret_cast<const unsigned char *>(in.constData()),
                          in.size()) != 1) {
        setError(collectErrors("文件流加解密失败"));
        reset();
        return QByteArray();
    }
    out.resize(len);
    return out;
}

QByteArray CryptoStream::finish()
{
    if (!isActive()) return QByteArray();

    QByteArray out(kKeyLen, '\0');
    int len = 0;
    const int rc = EVP_DecryptFinal_ex(static_cast<EVP_CIPHER_CTX *>(m_ctx),
                                       reinterpret_cast<unsigned char *>(out.data()), &len);
    QByteArray tail;
    if (rc == 1 && len > 0) {
        out.resize(len);
        tail = out;
    }
    reset();
    return tail;
}

// ==================================================================
// 链路层原语（P2 握手）
//
// 这一段在客户端与服务端是**逐字相同**的：握手两侧必须用同一套
// 字节序、同一套派生规则，否则算出来的密钥对不上。改动时务必两边同步。
// ==================================================================
namespace {

/// 全零检测：X25519 遇到低阶点会导出全零，这时必须中止而不是当密钥用
bool isAllZero(const QByteArray &data)
{
    unsigned char acc = 0;
    for (int i = 0; i < data.size(); ++i)
        acc |= static_cast<unsigned char>(data.at(i));
    return acc == 0;
}

/// 带 AAD 的 AES-256-GCM（单次，非流式）
bool mc2GcmSeal(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
                const QByteArray &plain, QByteArray *ct, QByteArray *tag)
{
    bool ok = false;
    QByteArray out(plain.size() + 16, 0);
    tag->resize(16);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    do {
        if (!ctx) break;
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1) break;
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                               reinterpret_cast<const unsigned char *>(key.constData()),
                               reinterpret_cast<const unsigned char *>(nonce.constData())) != 1) break;
        int len = 0;
        if (!aad.isEmpty()) {
            if (EVP_EncryptUpdate(ctx, nullptr, &len,
                                  reinterpret_cast<const unsigned char *>(aad.constData()),
                                  aad.size()) != 1) break;
        }
        int total = 0;
        if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &len,
                              reinterpret_cast<const unsigned char *>(plain.constData()),
                              plain.size()) != 1) break;
        total = len;
        if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total, &len) != 1) break;
        total += len;
        out.resize(total);
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag->data()) != 1) break;
        *ct = out;
        ok = true;
    } while (false);
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool mc2GcmOpen(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
                const QByteArray &ct, const QByteArray &tag, QByteArray *plain)
{
    bool ok = false;
    QByteArray out(ct.size() + 16, 0);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    do {
        if (!ctx) break;
        if (tag.size() != 16) break;
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1) break;
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                               reinterpret_cast<const unsigned char *>(key.constData()),
                               reinterpret_cast<const unsigned char *>(nonce.constData())) != 1) break;
        int len = 0;
        if (!aad.isEmpty()) {
            if (EVP_DecryptUpdate(ctx, nullptr, &len,
                                  reinterpret_cast<const unsigned char *>(aad.constData()),
                                  aad.size()) != 1) break;
        }
        int total = 0;
        if (EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &len,
                              reinterpret_cast<const unsigned char *>(ct.constData()),
                              ct.size()) != 1) break;
        total = len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                                const_cast<char *>(tag.constData())) != 1) break;
        if (EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total, &len) <= 0) break;
        total += len;
        out.resize(total);
        *plain = out;
        ok = true;
    } while (false);
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    return ok;
}

} // namespace

bool CryptoUtil::random(int len, QByteArray *out)
{
    if (!out || len <= 0) return false;
    const QByteArray buf = randomBytes(len);
    if (buf.size() != len) return false;
    *out = buf;
    return true;
}

QByteArray CryptoUtil::hmacSha256(const QByteArray &key, const QByteArray &data)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (HMAC(EVP_sha256(),
             key.constData(), key.size(),
             reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size()),
             md, &len) == nullptr) {
        setError(collectErrors("HMAC-SHA256 计算失败"));
        return QByteArray();
    }
    return QByteArray(reinterpret_cast<const char *>(md), int(len));
}

QByteArray CryptoUtil::pbkdf2Sha256(const QByteArray &pass, const QByteArray &salt,
                                    int iterations, int outLen)
{
    if (iterations <= 0 || outLen <= 0) return QByteArray();
    QByteArray out(outLen, 0);
    if (PKCS5_PBKDF2_HMAC(pass.constData(), pass.size(),
                          reinterpret_cast<const unsigned char *>(salt.constData()), salt.size(),
                          iterations, EVP_sha256(), outLen,
                          reinterpret_cast<unsigned char *>(out.data())) != 1) {
        setError(collectErrors("PBKDF2 计算失败"));
        return QByteArray();
    }
    return out;
}

bool CryptoUtil::constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    // 长度不同直接判否（长度本身不是秘密），内容逐字节异或累积，不提前返回
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    return diff == 0;
}

bool CryptoUtil::generateX25519KeyPair(QByteArray *priv, QByteArray *pub)
{
    clearError();
    if (!priv || !pub) return false;

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx) { setError(collectErrors("创建 X25519 上下文失败")); return false; }

    EVP_PKEY *pkey = nullptr;
    bool ok = false;
    do {
        if (EVP_PKEY_keygen_init(ctx) != 1) { setError(collectErrors("初始化 X25519 密钥生成失败")); break; }
        if (EVP_PKEY_keygen(ctx, &pkey) != 1) { setError(collectErrors("生成 X25519 密钥失败")); break; }

        QByteArray sk(32, 0), pk(32, 0);
        size_t skLen = 32, pkLen = 32;
        if (EVP_PKEY_get_raw_private_key(pkey, reinterpret_cast<unsigned char *>(sk.data()), &skLen) != 1) {
            setError(collectErrors("导出 X25519 私钥失败")); break;
        }
        if (EVP_PKEY_get_raw_public_key(pkey, reinterpret_cast<unsigned char *>(pk.data()), &pkLen) != 1) {
            setError(collectErrors("导出 X25519 公钥失败")); break;
        }
        sk.resize(int(skLen));
        pk.resize(int(pkLen));
        if (sk.size() != 32 || pk.size() != 32) { setError("X25519 密钥长度不是 32 字节"); break; }
        *priv = sk;
        *pub  = pk;
        ok = true;
    } while (false);

    if (pkey) EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
    return ok;
}

QByteArray CryptoUtil::x25519PublicFromPrivate(const QByteArray &priv)
{
    if (priv.size() != 32) return QByteArray();
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(
                EVP_PKEY_X25519, nullptr,
                reinterpret_cast<const unsigned char *>(priv.constData()), 32);
    if (!pkey) return QByteArray();

    QByteArray pk(32, 0);
    size_t len = 32;
    const bool ok = (EVP_PKEY_get_raw_public_key(
                         pkey, reinterpret_cast<unsigned char *>(pk.data()), &len) == 1);
    EVP_PKEY_free(pkey);
    if (!ok) return QByteArray();
    pk.resize(int(len));
    return pk;
}

bool CryptoUtil::x25519SharedSecret(const QByteArray &priv, const QByteArray &peerPub,
                                    QByteArray *secret)
{
    clearError();
    if (!secret) return false;
    if (priv.size() != 32 || peerPub.size() != 32) {
        setError("X25519 密钥长度不正确");
        return false;
    }

    EVP_PKEY *mine = EVP_PKEY_new_raw_private_key(
                EVP_PKEY_X25519, nullptr,
                reinterpret_cast<const unsigned char *>(priv.constData()), 32);
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(
                EVP_PKEY_X25519, nullptr,
                reinterpret_cast<const unsigned char *>(peerPub.constData()), 32);
    EVP_PKEY_CTX *ctx = nullptr;
    bool ok = false;
    do {
        if (!mine || !peer) { setError(collectErrors("加载 X25519 密钥失败")); break; }
        ctx = EVP_PKEY_CTX_new(mine, nullptr);
        if (!ctx) { setError(collectErrors("创建 X25519 派生上下文失败")); break; }
        if (EVP_PKEY_derive_init(ctx) != 1) { setError(collectErrors("初始化 X25519 派生失败")); break; }
        if (EVP_PKEY_derive_set_peer(ctx, peer) != 1) { setError(collectErrors("设置对端 X25519 公钥失败")); break; }

        size_t len = 0;
        if (EVP_PKEY_derive(ctx, nullptr, &len) != 1) { setError(collectErrors("计算共享密钥长度失败")); break; }
        QByteArray out(int(len), 0);
        if (EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char *>(out.data()), &len) != 1) {
            setError(collectErrors("导出 X25519 共享密钥失败")); break;
        }
        out.resize(int(len));
        if (out.isEmpty() || isAllZero(out)) {
            setError("对端 X25519 公钥是低阶点，共享密钥为全零");
            break;
        }
        *secret = out;
        ok = true;
    } while (false);

    if (ctx) EVP_PKEY_CTX_free(ctx);
    if (mine) EVP_PKEY_free(mine);
    if (peer) EVP_PKEY_free(peer);
    return ok;
}

QByteArray CryptoUtil::hkdfExtract(const QByteArray &salt, const QByteArray &ikm)
{
    // RFC 5869 §2.2：salt 缺省时取 HashLen 个 0
    const QByteArray s = salt.isEmpty() ? QByteArray(32, '\0') : salt;
    return hmacSha256(s, ikm);
}

QByteArray CryptoUtil::hkdfExpand(const QByteArray &prk, const QByteArray &info, int len)
{
    // RFC 5869 §2.3：T(i) = HMAC(PRK, T(i-1) ‖ info ‖ i)，i 从 1 起
    if (prk.isEmpty() || len <= 0 || len > 255 * 32) return QByteArray();
    QByteArray out;
    QByteArray block;
    for (int i = 1; out.size() < len; ++i) {
        block = hmacSha256(prk, block + info + QByteArray(1, static_cast<char>(i)));
        if (block.isEmpty()) return QByteArray();
        out.append(block);
    }
    out.truncate(len);
    return out;
}

bool CryptoUtil::aesGcmSeal(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
                            const QByteArray &plain, QByteArray *ct, QByteArray *tag)
{
    clearError();
    if (!ct || !tag) return false;
    if (key.size() != 32 || nonce.isEmpty() || nonce.size() > 16) {
        setError("AES-GCM 参数不合法");
        return false;
    }
    if (!mc2GcmSeal(key, nonce, aad, plain, ct, tag)) {
        setError(collectErrors("AES-GCM 加密失败"));
        return false;
    }
    return true;
}

bool CryptoUtil::aesGcmOpen(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
                            const QByteArray &ct, const QByteArray &tag, QByteArray *plain)
{
    clearError();
    if (!plain) return false;
    if (key.size() != 32 || nonce.isEmpty() || nonce.size() > 16 || tag.size() != 16) {
        setError("AES-GCM 参数不合法");
        return false;
    }
    if (!mc2GcmOpen(key, nonce, aad, ct, tag, plain)) {
        setError("AES-GCM 校验失败（密文被改或密钥不对）");
        return false;
    }
    return true;
}

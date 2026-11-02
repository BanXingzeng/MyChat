#include "cryptoutil.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
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
    return t.contains("\"alg\"") && t.contains("\"eks\"") && t.contains("\"ct\"");
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

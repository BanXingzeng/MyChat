#include "passwordhash.h"

#include <QByteArray>
#include <QStringList>
#include <QDebug>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {

const char *kPrefix     = "pbkdf2-sha256";
const int   kSaltLen    = 16;   // 128 位随机盐
const int   kDigestLen  = 32;   // SHA-256 输出

/// 定长比较，避免通过响应耗时逐字节猜摘要
bool constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    }
    return diff == 0;
}

/// PBKDF2-HMAC-SHA256
QByteArray pbkdf2(const QByteArray &plain, const QByteArray &salt, int iterations)
{
    QByteArray out(kDigestLen, Qt::Uninitialized);
    const int ok = PKCS5_PBKDF2_HMAC(plain.constData(), plain.size(),
                                     reinterpret_cast<const unsigned char *>(salt.constData()),
                                     salt.size(), iterations, EVP_sha256(),
                                     kDigestLen,
                                     reinterpret_cast<unsigned char *>(out.data()));
    if (ok != 1) {
        qWarning() << "[密码] PBKDF2 计算失败";
        return QByteArray();
    }
    return out;
}

} // namespace

namespace PasswordHash {

QString make(const QString &plain, int iterations)
{
    if (plain.isEmpty() || iterations <= 0) return QString();

    unsigned char saltBuf[kSaltLen];
    if (RAND_bytes(saltBuf, kSaltLen) != 1) {
        qWarning() << "[密码] 随机盐生成失败";
        return QString();
    }
    const QByteArray salt(reinterpret_cast<const char *>(saltBuf), kSaltLen);
    const QByteArray digest = pbkdf2(plain.toUtf8(), salt, iterations);
    if (digest.isEmpty()) return QString();

    return QString("%1$%2$%3$%4")
            .arg(QString::fromLatin1(kPrefix))
            .arg(iterations)
            .arg(QString::fromLatin1(salt.toBase64()))
            .arg(QString::fromLatin1(digest.toBase64()));
}

bool isHashed(const QString &stored)
{
    return stored.startsWith(QString::fromLatin1(kPrefix) + QLatin1Char('$'));
}

bool verify(const QString &plain, const QString &stored)
{
    if (plain.isEmpty() || stored.isEmpty()) return false;

    // 还没迁移的明文：直接比较，迁移完成后这个分支自然走不到
    if (!isHashed(stored)) {
        return constantTimeEquals(plain.toUtf8(), stored.toUtf8());
    }

    const QStringList parts = stored.split(QLatin1Char('$'));
    if (parts.size() != 4) {
        qWarning() << "[密码] 哈希格式不正确";
        return false;
    }

    bool ok = false;
    const int iterations = parts.at(1).toInt(&ok);
    if (!ok || iterations <= 0) return false;

    const QByteArray salt   = QByteArray::fromBase64(parts.at(2).toLatin1());
    const QByteArray expect = QByteArray::fromBase64(parts.at(3).toLatin1());
    if (salt.isEmpty() || expect.isEmpty()) return false;

    const QByteArray actual = pbkdf2(plain.toUtf8(), salt, iterations);
    if (actual.isEmpty()) return false;

    return constantTimeEquals(actual, expect);
}

} // namespace PasswordHash

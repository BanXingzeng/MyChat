#ifndef PASSWORDHASH_H
#define PASSWORDHASH_H

#include <QString>

/**
 * @brief 密码加盐哈希（PBKDF2-HMAC-SHA256）
 *
 * 存储格式（单列 passwd 内自带算法参数，方便以后调迭代次数而不破坏老数据）：
 *     pbkdf2-sha256$<迭代次数>$<盐 Base64>$<摘要 Base64>
 *
 * 设计要点：
 *  - 每个用户独立随机盐，同样的口令得到不同摘要，彩虹表失效；
 *  - 迭代 10 万次，单次校验约几十毫秒，暴力枚举成本被放大；
 *  - verify() 对"还没迁移的明文密码"返回真，配合 DataBaseMag 的启动迁移，
 *    老库不需要手工处理；
 *  - 比较过程用固定时间比较，避免按耗时猜摘要。
 */
namespace PasswordHash
{
    /// 把明文口令变成可入库的哈希串
    QString make(const QString &plain, int iterations = 100000);

    /// 校验明文口令与库里的存储值是否匹配（兼容未迁移的明文）
    bool verify(const QString &plain, const QString &stored);

    /// 判断一个存储值是否已经是哈希格式
    bool isHashed(const QString &stored);
}

#endif // PASSWORDHASH_H

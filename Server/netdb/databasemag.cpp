#include "databasemag.h"
#include <QDebug>
#include <QJsonObject>
#include <QSqlQuery>
#include "comapi/myapp.h"
#include "comapi/cryptoutil.h"
#include "passwordhash.h"
#include <QSqlError>
#include <QSet>
#include <QStringList>
#include <QPair>
#include <QVariant>

DataBaseMag *DataBaseMag::self = nullptr;

DataBaseMag::DataBaseMag(QObject *parent) : QObject(parent)
{
}

DataBaseMag::~DataBaseMag()
{
    if (userdb.isOpen()) {
        userdb.close();
    }
}

bool DataBaseMag::openDatabase(const QString &dataName)
{
    if (userdb.isOpen()) {
        return true;
    }
    if (QSqlDatabase::contains("qt_sql_default_connection")) {
        userdb = QSqlDatabase::database("qt_sql_default_connection");
    } else {
        userdb = QSqlDatabase::addDatabase("QSQLITE");
    }
    userdb.setDatabaseName(dataName);
    if (!userdb.open()) {
        // 打印完整错误信息
        qDebug() << "数据库打开失败："
                 << "路径=" << dataName
                 << "错误信息=" << userdb.lastError().text();
        return false;
    }
    QSqlQuery query(userdb);
    query.exec("PRAGMA foreign_keys = ON;");

    createDatabase();

    // 老版本数据库缺列时自动补齐（电话 / 工作组 / 邮箱）
    ensureUserInfoColumns();

    // 历史遗留的明文口令统一转成加盐哈希（只跑一次，之后没有明文可迁）
    migratePlainPasswords();

    qDebug() << "数据库打开成功：" << dataName;
    return true;
}

void DataBaseMag::closeDatabase()
{
    qDebug() << userdb;
    userdb.close();
}

void DataBaseMag::createDatabase()
{
    QSqlQuery query(userdb);
    QString sql;

    query.exec("PRAGMA foreign_keys = ON;");    // 启用SQLite外键约束

    sql = QString("CREATE TABLE IF NOT EXISTS USERINFO (");
    sql.append(QString("id INTEGER PRIMARY KEY AUTOINCREMENT,"));
    sql.append(QString("name VARCHAR(30) NOT NULL UNIQUE,"));
    sql.append(QString("passwd VARCHAR(200) NOT NULL,"));   // 存 PBKDF2 哈希串，不是明文
    sql.append(QString("head VARCHAR(100) DEFAULT 'default.png',"));
    sql.append(QString("phone VARCHAR(30) DEFAULT '',"));
    sql.append(QString("work_group VARCHAR(50) DEFAULT '',"));
    sql.append(QString("email VARCHAR(60) DEFAULT '',"));
    sql.append(QString("pub_key TEXT DEFAULT '',"));        // 用户公钥 PEM（公开信息，供他人加密）
    sql.append(QString("status INTEGER DEFAULT %1,").arg(OffLine));
    sql.append(QString("lasttime DATETIME DEFAULT NULL);"));
    query.exec(sql);

    sql = QString("CREATE TABLE IF NOT EXISTS GROUPS (");
    sql.append(QString("group_id INTEGER PRIMARY KEY AUTOINCREMENT,"));
    sql.append(QString("group_name VARCHAR(50) NOT NULL,"));
    sql.append(QString("group_head VARCHAR(100) DEFAULT 'default_group.png',"));
    sql.append(QString("create_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"));
    sql.append(QString("creator_id INTEGER NOT NULL REFERENCES USERINFO(id) ON DELETE CASCADE);"));
    query.exec(sql);

    sql = QString("CREATE TABLE IF NOT EXISTS GROUP_MEMBER (");
    sql.append(QString("id INTEGER PRIMARY KEY AUTOINCREMENT,"));
    sql.append(QString("group_id INTEGER NOT NULL REFERENCES GROUPS(group_id) ON DELETE CASCADE,"));
    sql.append(QString("user_id INTEGER NOT NULL REFERENCES USERINFO(id) ON DELETE CASCADE,"));
    sql.append(QString("identity INTEGER NOT NULL DEFAULT 0,"));
    sql.append(QString("join_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"));
    sql.append(QString("UNIQUE (group_id, user_id));"));
    query.exec(sql);

    sql = QString("CREATE TABLE IF NOT EXISTS FRIEND (");
    sql.append(QString("id INTEGER PRIMARY KEY AUTOINCREMENT,"));          // 好友记录ID（自增主键）
    sql.append(QString("user_id INTEGER NOT NULL,"));                     // 发起添加的用户ID（我的ID）
    sql.append(QString("friend_id INTEGER NOT NULL,"));                   // 被添加的好友ID（对方ID）
    sql.append(QString("remark VARCHAR(30) DEFAULT '',"));                // 好友备注名（自定义）
    sql.append(QString("status INTEGER NOT NULL DEFAULT 0,"));            // 好友状态（0=待验证/1=已通过/2=已拉黑）
    sql.append(QString("add_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,")); // 添加时间
    sql.append(QString("update_time DATETIME DEFAULT CURRENT_TIMESTAMP,"));// 状态更新时间
    // 外键关联USERINFO，级联删除
    sql.append(QString("FOREIGN KEY (user_id) REFERENCES USERINFO(id) ON DELETE CASCADE,"));
    sql.append(QString("FOREIGN KEY (friend_id) REFERENCES USERINFO(id) ON DELETE CASCADE,"));
    sql.append(QString("UNIQUE (user_id, friend_id));"));
    query.exec(sql);

    // 显式列名，避免后续给 USERINFO 加列后这里因列数不匹配而失败
    sql = QString("INSERT INTO USERINFO (id, name, passwd, head, status, lasttime) "
                  "VALUES(1, 'admin', ?, '2.bmp', %1, '');").arg(OffLine);    //插入数据
    query.prepare(sql);
    query.addBindValue(PasswordHash::make(QStringLiteral("123456")));   // 只存哈希，不落明文
    query.exec();   // 主键冲突时失败属正常（只有首次建库会真正插入）

    //消息队列数据库
    sql = QString ("CREATE TABLE IF NOT EXISTS MESSAGEQUEUE (");
    sql.append(QString("id INTEGER PRIMARY KEY AUTOINCREMENT,"));
    sql.append(QString("request_userId INTEGER NOT NULL,"));
    sql.append(QString("accept_userId INTEGER NOT NULL,"));
    sql.append(QString("status INTEGER NOT NULL DEFAULT 0,"));  //0=待处理/1=已处理
    sql.append(QString("data TEXT NOT NULL,"));                   //数据部分
    sql.append(QString("message_type INTEGER NOT NULL);"));

    query.exec(sql);

    // 消息审计日志：端到端加密下服务器只存密文原文
    sql = QString("CREATE TABLE IF NOT EXISTS MSGLOG (");
    sql.append(QString("id INTEGER PRIMARY KEY AUTOINCREMENT,"));
    sql.append(QString("send_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"));
    sql.append(QString("from_id INTEGER NOT NULL,"));
    sql.append(QString("to_id INTEGER NOT NULL,"));          // 私聊=接收者ID，群聊=群ID
    sql.append(QString("is_group INTEGER NOT NULL DEFAULT 0,"));
    sql.append(QString("msg_type INTEGER NOT NULL DEFAULT 0,"));
    sql.append(QString("payload TEXT NOT NULL);"));          // 消息 JSON 原样（密文字段不被改动）
    query.exec(sql);
    query.exec("CREATE INDEX IF NOT EXISTS idx_msglog_time ON MSGLOG(send_time);");

    // 说明：服务器不保存任何私钥，因此没有 SYSKEY 之类的密钥表。
    // 端到端加密要求服务器"零知识"：它只转发密文，永远不持有能解开正文的材料。
}

/**
 * @brief DataBaseMag::ensureUserInfoColumns
 * 老版本数据库的 USERINFO 表只有 id/name/passwd/head/status/lasttime，
 * 这里检查并补齐服务器管理界面用到的 phone / work_group / email 三列。
 */
void DataBaseMag::ensureUserInfoColumns()
{
    QSqlQuery query(userdb);
    QSet<QString> columns;
    if (query.exec("PRAGMA table_info(USERINFO);")) {
        while (query.next()) {
            columns.insert(query.value("name").toString().toLower());
        }
    }

    if (!columns.contains("phone")) {
        if (query.exec("ALTER TABLE USERINFO ADD COLUMN phone VARCHAR(30) DEFAULT '';")) {
            qInfo() << "[数据库] USERINFO 补充 phone 列";
        } else {
            qWarning() << "[数据库] 补充 phone 列失败：" << query.lastError().text();
        }
    }
    if (!columns.contains("work_group")) {
        if (query.exec("ALTER TABLE USERINFO ADD COLUMN work_group VARCHAR(50) DEFAULT '';")) {
            qInfo() << "[数据库] USERINFO 补充 work_group 列";
        } else {
            qWarning() << "[数据库] 补充 work_group 列失败：" << query.lastError().text();
        }
    }
    if (!columns.contains("email")) {
        if (query.exec("ALTER TABLE USERINFO ADD COLUMN email VARCHAR(60) DEFAULT '';")) {
            qInfo() << "[数据库] USERINFO 补充 email 列";
        } else {
            qWarning() << "[数据库] 补充 email 列失败：" << query.lastError().text();
        }
    }
    if (!columns.contains("pub_key")) {
        if (query.exec("ALTER TABLE USERINFO ADD COLUMN pub_key TEXT DEFAULT '';")) {
            qInfo() << "[数据库] USERINFO 补充 pub_key 列";
        } else {
            qWarning() << "[数据库] 补充 pub_key 列失败：" << query.lastError().text();
        }
    }
}

/**
 * @brief DataBaseMag::migratePlainPasswords
 * 把历史遗留的明文口令就地转成 PBKDF2 加盐哈希。
 *
 * 老库（含早期注册的用户、内置 admin）存的是明文，直接换成哈希会导致
 * 这些账号无法登录，所以启动时自动迁移一次：逐个取出来算哈希再写回。
 * 密码本身不变，用户无感知。
 */
int DataBaseMag::migratePlainPasswords()
{
    if (!userdb.isOpen()) return 0;

    QList<QPair<int, QString>> plainList;   // id → 明文口令
    QSqlQuery query(userdb);
    if (!query.exec("SELECT id, passwd FROM USERINFO;")) {
        qDebug() << "[密码] 读取用户表失败：" << query.lastError().text();
        return 0;
    }
    while (query.next()) {
        const QString pwd = query.value("passwd").toString();
        if (!pwd.isEmpty() && !PasswordHash::isHashed(pwd)) {
            plainList.append(qMakePair(query.value("id").toInt(), pwd));
        }
    }
    if (plainList.isEmpty()) return 0;

    QSqlQuery update(userdb);
    update.prepare("UPDATE USERINFO SET passwd = :passwd WHERE id = :id;");

    int done = 0;
    for (const QPair<int, QString> &item : plainList) {
        const QString hashed = PasswordHash::make(item.second);
        if (hashed.isEmpty()) continue;

        update.bindValue(":passwd", hashed);
        update.bindValue(":id", item.first);
        if (update.exec()) {
            ++done;
        } else {
            qWarning() << "[密码] 迁移失败 id =" << item.first
                       << update.lastError().text();
        }
    }

    qInfo() << "[密码] 明文口令已迁移为 PBKDF2-SHA256 哈希，共" << done << "条";
    return done;
}

void DataBaseMag::changeAllUserStatus()
{
    QSqlQuery query;
    const QString sql = "UPDATE USERINFO SET status = ?";
    query.prepare(sql);
    query.addBindValue(OffLine);
    query.exec();
}


E_STATUS DataBaseMag::userRegister(const QString &name, const QString &passwd)
{
    QSqlQuery query;
    QString sql("INSERT INTO USERINFO (name, passwd) VALUES (?, ?)");
    query.prepare(sql);
    query.addBindValue(name);
    query.addBindValue(PasswordHash::make(passwd));   // 入库即哈希
    return query.exec() ? RegisterOk : RegisterFailed;
}

QJsonObject DataBaseMag::userLogin(const QString &name, const QString &passwd)
{
    QJsonObject jsonObj;
    int nId = -1;
    int code = -1;
    QString strHead = "0.bmp";

    QSqlQuery query;
    // 密码是加盐哈希，没法再用 WHERE passwd = ? 比较，只能取出来本地校验
    const QString sql = "SELECT id, head, status, passwd FROM USERINFO WHERE name = ?";

    query.prepare(sql);
    query.addBindValue(name);

    if (query.exec() && query.next()
            && PasswordHash::verify(passwd, query.value("passwd").toString())) {
        nId = query.value("id").toInt();
        int nStatus = query.value("status").toInt();
        if (OnLine == nStatus)
        {
            // 数据库里记的是在线，但真实情况要以连接表为准，
            // 这里把真实 id 一并带出去，让上层判断是不是脏数据
            jsonObj.insert("realId", nId);
            nId = -2;
            code = -2;
        }
        else
        {
            UpdateUserStatus(nId, OnLine);
            code = 0;
        }
        strHead = query.value("head").toString();
    }
    // 组织返回
    jsonObj.insert("id", nId);
    if(nId == -1){  //用户名或密码错误
        jsonObj.insert("msg", "noneUser");
    }else if(nId == -2){ //已在线
        jsonObj.insert("msg", "OnLine");
    }else {
        jsonObj.insert("msg", "ok");
    }
    jsonObj.insert("head", strHead);
    jsonObj.insert("code", code);

    return jsonObj;
}

/**
 * @brief DataBaseMag::authParams
 * 挑战-应答登录用：把库里的 pbkdf2-sha256$iter$salt$digest 拆开交给上层。
 *
 * 注意 digest 这一项就是协议里的 verifier，服务端用它对客户端发来的
 * proof 做一次 HMAC 重算即可，**不需要、也拿不到口令原文**。
 * 若历史数据还是明文（迁移未跑或迁移失败），这里返回 false ——
 * 明文没法参与挑战-应答，只能等迁移完成后才能登录。
 */
bool DataBaseMag::authParams(const QString &name, int *uid, QByteArray *salt,
                             int *iterations, QByteArray *verifier)
{
    if (uid) *uid = -1;

    QSqlQuery query;
    query.prepare("SELECT id, passwd FROM USERINFO WHERE name = ?");
    query.addBindValue(name);
    if (!query.exec() || !query.next()) return false;

    const QString stored = query.value("passwd").toString();
    if (!PasswordHash::isHashed(stored)) {
        qWarning() << "[认证] 用户" << name << "的口令还未迁移成哈希，无法走挑战-应答";
        return false;
    }

    const QStringList parts = stored.split(QLatin1Char('$'));
    if (parts.size() != 4) return false;

    bool ok = false;
    const int iter = parts.at(1).toInt(&ok);
    if (!ok || iter <= 0) return false;

    const QByteArray s = QByteArray::fromBase64(parts.at(2).toLatin1());
    const QByteArray v = QByteArray::fromBase64(parts.at(3).toLatin1());
    if (s.isEmpty() || v.isEmpty()) return false;

    if (uid) *uid = query.value("id").toInt();
    if (salt) *salt = s;
    if (iterations) *iterations = iter;
    if (verifier) *verifier = v;
    return true;
}

/**
 * @brief DataBaseMag::loginResultById
 * 口令已由挑战-应答证明过，这里只按 uid 走原来的"在线/顶号"判定。
 * 返回结构与 userLogin 保持一致，上层不必区分两条路径。
 */
QJsonObject DataBaseMag::loginResultById(int uid)
{
    QJsonObject jsonObj;
    int nId = -1;
    int code = -1;
    QString strHead = "0.bmp";

    QSqlQuery query;
    query.prepare("SELECT head, status FROM USERINFO WHERE id = ?");
    query.addBindValue(uid);
    if (query.exec() && query.next()) {
        strHead = query.value("head").toString();
        const int nStatus = query.value("status").toInt();
        if (OnLine == nStatus) {
            // 数据库记着在线，真实情况由连接表判定，把 realId 带出去
            jsonObj.insert("realId", uid);
            nId = -2;
            code = -2;
        } else {
            UpdateUserStatus(uid, OnLine);
            nId = uid;
            code = 0;
        }
    }

    jsonObj.insert("id", nId);
    if (nId == -1)      jsonObj.insert("msg", "noneUser");
    else if (nId == -2) jsonObj.insert("msg", "OnLine");
    else                jsonObj.insert("msg", "ok");
    jsonObj.insert("head", strHead);
    jsonObj.insert("code", code);

    return jsonObj;
}

bool DataBaseMag::isFriend(const int &userId, const QString &friendName){
    QSqlQuery query;
    const QString sql = "SELECT 1 FROM FRIEND "
                        "WHERE user_id = :user_id "
                        "AND friend_id = (SELECT id FROM USERINFO WHERE name = :friendName) "
                        "LIMIT 1";
    query.prepare(sql);
    query.bindValue(":user_id", userId);
    query.bindValue(":friendName", friendName);

    if(!query.exec()){
        qDebug() << "判断是否为好友失败";
        return false;
    }
    return query.next();

}

bool DataBaseMag::isFriendById(int userId, int friendId)
{
    if (userId <= 0 || friendId <= 0 || userId == friendId) return false;

    QSqlQuery query;
    query.prepare("SELECT 1 FROM FRIEND "
                  "WHERE user_id = :user_id AND friend_id = :friend_id LIMIT 1");
    query.bindValue(":user_id", userId);
    query.bindValue(":friend_id", friendId);

    if (!query.exec()) {
        qDebug() << "[授权] 查询好友关系失败" << query.lastError();
        return false;
    }
    return query.next();
}

bool DataBaseMag::isGroupMember(int groupId, int userId)
{
    if (groupId <= 0 || userId <= 0) return false;

    QSqlQuery query;
    query.prepare("SELECT 1 FROM GROUP_MEMBER "
                  "WHERE group_id = :group_id AND user_id = :user_id LIMIT 1");
    query.bindValue(":group_id", groupId);
    query.bindValue(":user_id", userId);

    if (!query.exec()) {
        qDebug() << "[授权] 查询群成员失败" << query.lastError();
        return false;
    }
    return query.next();
}

/**
 * @brief DataBaseMag::updateUserHead
 * 更新用户头像文件名（只存文件名，不存完整路径）
 */
bool DataBaseMag::updateUserHead(const int &userId, const QString &head)
{
    if (userId < 0 || head.isEmpty()) {
        qDebug() << "[头像] 更新失败，参数不合法 userId =" << userId << " head =" << head;
        return false;
    }

    QSqlQuery query;
    query.prepare("UPDATE USERINFO SET head = :head WHERE id = :userId;");
    query.bindValue(":head", head);
    query.bindValue(":userId", userId);
    if (!query.exec()) {
        qDebug() << "[头像] 更新数据库失败" << query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

QString DataBaseMag::getUserHead(const int &userId)
{
    QSqlQuery query;
    QString sql = "SELECT head FROM USERINFO WHERE id = :userId;";
    query.prepare(sql);
    query.bindValue(":userId", userId);
    if(!query.exec())
    {
        qDebug() << "获取用户头像路径错误" << query.lastError();
        return "";
    }
    if(!query.next()){
        qDebug() << "没有查询到该用户";
        return  "";
    }
    return query.value("head").toString();
}

/**
 * @brief DataBaseMag::insertUser
 * 服务器管理界面添加用户（带电话 / 工作组 / 邮箱 / 头像），返回新用户ID，失败返回 -1
 */
int DataBaseMag::insertUser(const QString &name, const QString &passwd,
                            const QString &phone, const QString &workGroup,
                            const QString &email, const QString &head, QString *err)
{
    auto fail = [err](const QString &msg) {
        if (err) *err = msg;
        return -1;
    };
    const QString userName = name.trimmed();
    if (userName.isEmpty()) return fail("用户名不能为空");
    if (passwd.isEmpty())    return fail("密码不能为空");
    if (haveUser(userName))  return fail("用户名已存在");

    QSqlQuery query(userdb);
    query.prepare("INSERT INTO USERINFO (name, passwd, phone, work_group, email, head, status) "
                  "VALUES (:name, :passwd, :phone, :wg, :email, :head, :status);");
    query.bindValue(":name", userName);
    query.bindValue(":passwd", PasswordHash::make(passwd));   // 只存哈希
    query.bindValue(":phone", phone);
    query.bindValue(":wg", workGroup);
    query.bindValue(":email", email);
    query.bindValue(":head", head.isEmpty() ? QStringLiteral("default.png") : head);
    query.bindValue(":status", static_cast<int>(OffLine));
    if (!query.exec()) {
        qDebug() << "[用户] 添加失败：" << query.lastError().text();
        return fail(query.lastError().text());
    }

    int newId = query.lastInsertId().toInt();
    qInfo() << "[用户] 添加成功：" << userName << " id =" << newId;
    return newId;
}

/**
 * @brief DataBaseMag::updateUserInfo
 * 修改用户资料；head 为空表示不修改头像
 */
bool DataBaseMag::updateUserInfo(int id, const QString &name, const QString &passwd,
                                 const QString &phone, const QString &workGroup,
                                 const QString &email, const QString &head, QString *err)
{
    auto fail = [err](const QString &msg) {
        if (err) *err = msg;
        return false;
    };
    if (id <= 0) return fail("请先选择一个用户");
    const QString userName = name.trimmed();
    if (userName.isEmpty()) return fail("用户名不能为空");

    QSqlQuery dup(userdb);
    dup.prepare("SELECT id FROM USERINFO WHERE name = :name AND id <> :id;");
    dup.bindValue(":name", userName);
    dup.bindValue(":id", id);
    if (dup.exec() && dup.next()) return fail("用户名已被其它用户占用");

    // 密码列存的是哈希，界面无法回填原密码，因此"留空"表示保持原密码不变
    QStringList sets;
    sets << "name = :name" << "phone = :phone"
         << "work_group = :wg" << "email = :email";
    if (!head.isEmpty())     sets << "head = :head";
    if (!passwd.isEmpty())   sets << "passwd = :passwd";

    QSqlQuery query(userdb);
    query.prepare(QString("UPDATE USERINFO SET %1 WHERE id = :id;").arg(sets.join(", ")));
    if (!head.isEmpty())     query.bindValue(":head", head);
    if (!passwd.isEmpty())   query.bindValue(":passwd", PasswordHash::make(passwd));
    query.bindValue(":name", userName);
    query.bindValue(":phone", phone);
    query.bindValue(":wg", workGroup);
    query.bindValue(":email", email);
    query.bindValue(":id", id);
    if (!query.exec()) {
        qDebug() << "[用户] 修改失败：" << query.lastError().text();
        return fail(query.lastError().text());
    }
    qInfo() << "[用户] 修改成功：id =" << id << " name =" << userName;
    return true;
}

/**
 * @brief DataBaseMag::deleteUser
 * 删除用户，好友关系与群成员记录由外键级联删除
 */
bool DataBaseMag::deleteUser(int id, QString *err)
{
    auto fail = [err](const QString &msg) {
        if (err) *err = msg;
        return false;
    };
    if (id <= 0) return fail("请先选择一个用户");
    if (id == 1) return fail("内置管理员 admin 不允许删除");

    QSqlQuery query(userdb);
    query.prepare("DELETE FROM USERINFO WHERE id = :id;");
    query.bindValue(":id", id);
    if (!query.exec()) {
        qDebug() << "[用户] 删除失败：" << query.lastError().text();
        return fail(query.lastError().text());
    }
    if (query.numRowsAffected() <= 0) return fail("该用户不存在或已被删除");
    qInfo() << "[用户] 已删除：id =" << id;
    return true;
}

/**
 * @brief DataBaseMag::checkPassword
 * 只校验密码，不修改在线状态（区别于 userLogin）
 */
bool DataBaseMag::checkPassword(const QString &name, const QString &passwd)
{
    if (name.isEmpty() || passwd.isEmpty()) return false;
    QSqlQuery query(userdb);
    query.prepare("SELECT passwd FROM USERINFO WHERE name = :name LIMIT 1;");
    query.bindValue(":name", name);
    if (!query.exec() || !query.next()) return false;
    return PasswordHash::verify(passwd, query.value("passwd").toString());
}

bool DataBaseMag::updatePassword(const QString &name, const QString &newPasswd)
{
    if (name.isEmpty() || newPasswd.isEmpty()) return false;
    QSqlQuery query(userdb);
    query.prepare("UPDATE USERINFO SET passwd = :passwd WHERE name = :name;");
    query.bindValue(":passwd", PasswordHash::make(newPasswd));
    query.bindValue(":name", name);
    if (!query.exec()) {
        qDebug() << "[密码] 修改失败：" << query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

/**
 * @brief DataBaseMag::getUserInfoByName
 * 按用户名（支持模糊）或纯数字ID查询单个用户详情，未找到返回空 map
 */
QVariantMap DataBaseMag::getUserInfoByName(const QString &key)
{
    QVariantMap map;
    const QString k = key.trimmed();
    if (k.isEmpty() || !userdb.isOpen()) return map;

    QSqlQuery query(userdb);
    bool isId = false;
    const int id = k.toInt(&isId);
    if (isId) {
        query.prepare("SELECT * FROM USERINFO WHERE id = :v LIMIT 1;");
        query.bindValue(":v", id);
    } else {
        query.prepare("SELECT * FROM USERINFO WHERE name = :v OR name LIKE :like "
                      "ORDER BY (name = :v) DESC LIMIT 1;");
        query.bindValue(":v", k);
        query.bindValue(":like", "%" + k + "%");
    }
    if (!query.exec()) {
        qDebug() << "[用户] 查询失败：" << query.lastError().text();
        return map;
    }
    if (!query.next()) return map;

    map["id"]         = query.value("id").toInt();
    map["name"]       = query.value("name").toString();
    map["passwd"]     = QString();   // 只存哈希，不回传（界面留空=不修改）
    map["head"]       = query.value("head").toString();
    map["phone"]      = query.value("phone").toString();
    map["work_group"] = query.value("work_group").toString();
    map["email"]      = query.value("email").toString();
    map["status"]     = query.value("status").toInt();
    map["lasttime"]   = query.value("lasttime").toString();
    return map;
}

QVariantMap DataBaseMag::getUserInfoById(int id)
{
    if (id <= 0) return QVariantMap();
    return getUserInfoByName(QString::number(id));
}

int DataBaseMag::getUserCount()
{
    QSqlQuery query(userdb);
    if (query.exec("SELECT COUNT(*) FROM USERINFO;") && query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

int DataBaseMag::getOnlineUserCount()
{
    QSqlQuery query(userdb);
    query.prepare("SELECT COUNT(*) FROM USERINFO WHERE status = :status;");
    query.bindValue(":status", static_cast<int>(OnLine));
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

int DataBaseMag::getFriendCount(int userId)
{
    if (userId <= 0) return 0;
    QSqlQuery query(userdb);
    query.prepare("SELECT COUNT(*) FROM FRIEND WHERE user_id = :id;");
    query.bindValue(":id", userId);
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

int DataBaseMag::getGroupCount(int userId)
{
    if (userId <= 0) return 0;
    QSqlQuery query(userdb);
    query.prepare("SELECT COUNT(*) FROM GROUP_MEMBER WHERE user_id = :id;");
    query.bindValue(":id", userId);
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

bool DataBaseMag::haveUser(const QString &name)
{
    if(name.isEmpty()){
        return false;
    }
    QSqlQuery query;
    const QString sql("SELECT name FROM USERINFO where name = ?");
    query.prepare(sql);
    query.addBindValue(name);

    if(!query.exec()){
        return false;
    }
    return query.next();
}

void DataBaseMag::UpdateUserStatus(int id, E_STATUS status, bool touchTime)
{
    if (id <= 0) return;

    QSqlQuery query;
    const QString sql = touchTime
            ? QString("UPDATE USERINFO SET status = ?, lasttime = datetime('now') WHERE id = ?")
            : QString("UPDATE USERINFO SET status = ? WHERE id = ?");
    query.prepare(sql);
    query.addBindValue(static_cast<int>(status));
    query.addBindValue(id);
    query.exec();
}

QList<QVariantMap> DataBaseMag::getAllUser()
{
    QList<QVariantMap> userList;
    if (!userdb.isOpen()) {
        qDebug() << "查询失败：数据库未打开";
        return userList;
    }

    QSqlQuery query(userdb);
    QString sql = "SELECT * FROM USERINFO;";

    if (query.exec(sql)) {
        while (query.next()) {
            QVariantMap userMap;
            // 按字段名取值，键名和数据库字段一致，便于后续使用
            userMap["id"] = query.value("id").toInt();
            userMap["name"] = query.value("name").toString();
            userMap["passwd"] = QString();   // 密码哈希不外泄到界面
            userMap["head"] = query.value("head").toString();
            userMap["phone"] = query.value("phone").toString();
            userMap["work_group"] = query.value("work_group").toString();
            userMap["email"] = query.value("email").toString();
            userMap["status"] = query.value("status").toInt();
            userMap["lasttime"] = query.value("lasttime").toString();

            userList.append(userMap); // 添加到列表
        }
    } else {
        qDebug() << "查询失败：" << query.lastError().text();
    }

    return userList;
}

int DataBaseMag::getIdFromUsername(const QString &userName)
{
    QSqlQuery query;
    QString sql("select id from userinfo where name = :name");
    query.prepare(sql);
    query.bindValue(":name", userName);
    if(!query.exec()){
        qDebug() << "添加好友：没有该用户";
        return -1;
    }
    if(!query.next()) return -1;
    return query.value("id").toInt();
}


QString DataBaseMag::getUsernameFromId(const QString &userId)
{
    if(userId.isEmpty()) {
        qDebug() << "userId不能为空";
        return "";
    }
    QSqlQuery query;
    QString sql("select name from userinfo where id = :userId");
    query.prepare(sql);
    query.bindValue(":userId", userId);
    if(!query.exec()){
        qDebug() << "通过id获取用户名失败";
        return "";
    }
    if(!query.next()){
        qDebug() << "userId:" << userId << "无匹配数据：userId=" << userId;
        return "";
    }
    return query.value("name").toString();
}

QString DataBaseMag::jsonValueToString(const QJsonValue &jsonVal)
{
    if (jsonVal.isNull() || jsonVal.isUndefined()) {
        qWarning() << "QJsonValue为空或未定义，转换失败";
        return "";
    }

    QJsonDocument jsonDoc;
    if (jsonVal.isObject()) {
        jsonDoc = QJsonDocument(jsonVal.toObject());
    } else if (jsonVal.isArray()) {
        jsonDoc = QJsonDocument(jsonVal.toArray());
    } else {
        jsonDoc = QJsonDocument(QJsonArray{jsonVal});
    }

    return jsonDoc.toJson(QJsonDocument::Compact);
}

void DataBaseMag::insertMessageQueue(const int &senderId, const int &acceptId, quint8 type, const QJsonValue &dataVal)
{
    qDebug() << dataVal;
    if(senderId < 0 || acceptId < 0){
        qDebug() << "无效的消息不能插入消息队列";
        return;
    }
    QString data = jsonValueToString(dataVal);
    QSqlQuery query;
    QString sql("INSERT INTO MESSAGEQUEUE (request_userId, accept_userId, data, message_type)"
                " VALUES (:request_userId, :accept_userId, :data, :message_type);");
    query.prepare(sql);
    query.bindValue(":request_userId",senderId);
    query.bindValue(":accept_userId",acceptId);
    query.bindValue(":data",data);
    query.bindValue(":message_type",static_cast<int>(type));

    if(!query.exec()){
        qDebug() << "消息队列插入数据失败：" << "senderId = " << senderId
                 << "accessId = " << acceptId
                 << "message_type = " << static_cast<int>(type)
                 << "data:" << data;
        return;
    }
}

QList<QVariantMap> DataBaseMag::getUserMessageQueue(const int &userId)
{
    QList<QVariantMap> msgList;
    QStringList  needUpdateIds;
    QSqlQuery query;
    QString sql("SELECT id, request_userId, accept_userId, data, message_type "
                "FROM MESSAGEQUEUE WHERE accept_userId = :userId AND status = 0;");
    query.prepare(sql);
    query.bindValue(":userId",userId);
    if(!query.exec()){
        qDebug() << "获取用户的消息队列失败";
        return msgList;
    }
    while(query.next()){
        //取出json
        QString jsonStr = query.value("data").toString();
        QByteArray jsonByte = jsonStr.toUtf8();
        QJsonParseError error;
        QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonByte, &error);
        if (error.error != QJsonParseError::NoError) {
            qCritical() << "JSON解析失败：" << error.errorString();
            return msgList;
        }
        QVariant  jsonVar = jsonDoc.toVariant();
        QJsonValue jsonVal = QJsonValue::fromVariant(jsonVar);

        QString id = query.value("id").toString();
        QVariantMap msgMap;
        msgMap["request_userId"] = query.value("request_userId");
        msgMap["accept_userId"] = query.value("accept_userId");
        msgMap["data"] = jsonVal;
        msgMap["message_type"] = query.value("message_type");

        msgList.append(msgMap);
        needUpdateIds.append(id);
    }
    if(needUpdateIds.isEmpty()){
        qDebug() << "获取用户消息队列：没有要更新的状态";
        return msgList;
    }

    sql = QString("UPDATE MESSAGEQUEUE SET status = 1 where id in (%1);").arg(needUpdateIds.join(","));
    QSqlQuery updateQuery(sql);
    if(!updateQuery.exec()){
        qDebug() << "获取用户消息队列：SQL更新语句有误";
    }

    return msgList;
}

void DataBaseMag::addFriend(const int &userId, const int &friendId)
{
    QSqlQuery query;
    QString sql;
    sql = QString("INSERT INTO FRIEND (user_id, friend_id, remark, status, add_time, update_time)");
    sql.append("VALUES (:userId, :friendId, :remark, 1, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP);");

    query.prepare(sql);
    query.bindValue(":userId",userId);
    query.bindValue(":friendId",friendId);
    query.bindValue(":remark",DataBaseMag::getInstance()->getUsernameFromId(QString::number(friendId)));

    if(!query.exec()){
        qDebug() << "服务器存储好友信息出错";
        return;
    }
    return;
}

void DataBaseMag::deleteFriend(const int &userId, const int &friendId)
{
    QSqlQuery query;
    QString sql;
    sql = QString("DELETE FROM FRIEND WHERE user_id = :userId AND friend_id = :friendId");
    query.prepare(sql);
    query.bindValue(":userId", userId);
    query.bindValue(":friendId", friendId);
    if(!query.exec()){
        qDebug() << "从数据库删除好友出错" << query.lastError();
        return;
    }
}

bool DataBaseMag::isOnline(const int &userId)
{
    QSqlQuery query;
    QString sql;
    sql = QString("SELECT status FROM USERINFO WHERE id = :userId");
    query.prepare(sql);
    query.bindValue(":userId",userId);

    if(!query.exec()){
        qDebug() << "获取用户在线状态失败";
        return true;    //失败就假设为在线
    }
    if(!query.next()){
        qDebug() << "用户不存在";    //不存在就假设为在线
        return true;
    }
    return query.value("status").toInt() == OnLine ? true : false;
}

// 发送Jsonarray  status  head    name    id
QJsonValue DataBaseMag::getMyFriends(const int &userId)
{
    QJsonArray jsonArr;
    QSqlQuery query;
    QString sql;
    sql = QString("SELECT status, head, name, id FROM USERINFO ");
    sql.append("WHERE id IN (SELECT friend_id FROM FRIEND WHERE user_id = :userId);");
    query.prepare(sql);
    query.bindValue(":userId", userId);

    if(!query.exec()){
        qDebug() << "获取用户好友失败 sql执行出错" << query.lastError();;
    }
    while(query.next()){
        QJsonObject jsonObj;
        jsonObj.insert("status",query.value("status").toInt());
        jsonObj.insert("head", query.value("head").toString());
        jsonObj.insert("name", query.value("name").toString());
        jsonObj.insert("id", query.value("id").toInt());
        jsonArr.append(jsonObj);
    }

    QJsonValue sendJsonVal(jsonArr);
    return sendJsonVal;
}

QJsonValue DataBaseMag::getMyGroup(const int &userId)
{
    QJsonArray jsonArr;
    QSqlQuery query;
//    QString sql = QString("SELECT group_id, identity FROM GROUP_MEMBER WHERE user_id = :userId;");
    QString sql = QString("SELECT gm.group_id, gm.identity, g.group_head, g.group_name, g.creator_id ");
    sql.append("FROM GROUP_MEMBER gm LEFT JOIN GROUPS g ON gm.group_id = g.group_id ");
    sql.append("WHERE gm.user_id = :userId");
    query.prepare(sql);
    query.bindValue(":userId", userId);
    if(!query.exec()) {
        qDebug() << "获取小组信息失败" << query.lastError();
    }
    while(query.next()){
        QJsonObject jsonObj;
        jsonObj.insert("id", query.value("group_id").toInt());
        jsonObj.insert("identity", query.value("identity").toInt());
        jsonObj.insert("head", query.value("group_head").toString());
        jsonObj.insert("name", query.value("group_name").toString());
        jsonObj.insert("owner", query.value("creator_id").toInt());
        jsonArr.append(jsonObj);
    }
    return QJsonValue(jsonArr);
}

int DataBaseMag::addGroup(const int &userId, const QString groupName)
{
    if(userId < 0 || groupName.isEmpty()){
        qDebug() << "数据库插入群组失败";
        return 0;
    }
    QSqlQuery query(userdb);
    QString sql;
    sql = QString("INSERT INTO GROUPS (group_name, creator_id) ");
    sql.append("VALUES (:groupName, :creatorId);");
    query.prepare(sql);
    query.bindValue(":groupName", groupName);
    query.bindValue(":creatorId", userId);
    if(!query.exec()){
        qDebug() << "添加群组出错" << query.lastError();
        return 0;
    }
    QVariant insertId = query.lastInsertId();
    if (!insertId.isValid()) {
        qDebug() << "获取group_id失败：ID无效";
        return 0;
    }
    qlonglong groupIdLL = insertId.toLongLong();
    int groupId = static_cast<int>(groupIdLL);

//    qDebug() << "插入成功，group_id=" << groupId; // 打印验证
    addGroupMember(userId, groupId, Owner);
    return groupId;
}

bool DataBaseMag::addGroupMember(const int &userId, const int &groupId, GroupIdentity identity)
{
    if(userId < 0 || groupId < 0) return false;
    QSqlQuery query;
    QString sql = "INSERT INTO GROUP_MEMBER (group_id, user_id, identity) VALUES (:groupId, :userId, :identity);";
    query.prepare(sql);
    query.bindValue(":groupId", groupId);
    query.bindValue(":userId", userId);
    query.bindValue(":identity", identity);
    if(!query.exec()){
        qDebug() << "添加群成员错误" << query.lastError();
        return false;
    }
    return true;
}

/**
 * @brief DataBaseMag::removeGroupMember
 * 退出群组：把某个成员从群里删掉
 */
bool DataBaseMag::removeGroupMember(const int &groupId, const int &userId)
{
    if (groupId < 0 || userId < 0) return false;
    QSqlQuery query;
    query.prepare("DELETE FROM GROUP_MEMBER WHERE group_id = :groupId AND user_id = :userId;");
    query.bindValue(":groupId", groupId);
    query.bindValue(":userId", userId);
    if (!query.exec()) {
        qDebug() << "删除群成员失败" << query.lastError();
        return false;
    }
    return true;
}

/**
 * @brief DataBaseMag::removeGroup
 * 解散群组：删除群以及它的所有成员记录
 */
bool DataBaseMag::removeGroup(const int &groupId)
{
    if (groupId < 0) return false;

    QSqlQuery query;
    query.prepare("DELETE FROM GROUP_MEMBER WHERE group_id = :groupId;");
    query.bindValue(":groupId", groupId);
    if (!query.exec()) {
        qDebug() << "删除群成员记录失败" << query.lastError();
        return false;
    }

    query.prepare("DELETE FROM GROUPS WHERE group_id = :groupId;");
    query.bindValue(":groupId", groupId);
    if (!query.exec()) {
        qDebug() << "删除群组失败" << query.lastError();
        return false;
    }
    return true;
}

int DataBaseMag::getGroupOwner(const int &groupId)
{
    QSqlQuery query;
    QString sql = "SELECT creator_id from GROUPS WHERE group_id = :groupId";
    query.prepare(sql);
    query.bindValue(":groupId", groupId);
    if(!query.exec()){
        qDebug() << "获取群主Id错误" << query.lastError();
        return -1;
    }
    query.next();
    return query.value("creator_id").toInt();
}

QString DataBaseMag::getGroupName(const int &groupId)
{
    QSqlQuery query;
    QString sql = "SELECT group_name FROM GROUPS WHERE group_id = :groupId";
    query.prepare(sql);
    query.bindValue(":groupId", groupId);
    if(!query.exec()){
        qDebug() << "获取群组名错误" << query.lastError();
        return "";
    }
    query.next();
    return query.value("group_name").toString();
}

QString DataBaseMag::getGroupHead(const int &groupId)
{
    QSqlQuery query;
    QString sql = "SELECT group_head FROM GROUPS WHERE group_id = :groupId";
    query.prepare(sql);
    query.bindValue(":groupId", groupId);
    if(!query.exec()){
        qDebug() << "获取群组头像错误" << query.lastError();
        return "";
    }
    query.next();
    return query.value("group_head").toString();
}

QHash<int, QSet<int> > DataBaseMag::initGroupMembersCache()
{
    QHash<int, QSet<int> > hashSet;
    QString sql = "SELECT group_id, user_id FROM GROUP_MEMBER;";
    QSqlQuery query(sql);
    if(!query.exec()) {
        qDebug() << "初始化群组成员错误";
        return hashSet;
    }
    while(query.next()) {
        int groupId = query.value(0).toInt();
        int userId = query.value(1).toInt();
        hashSet[groupId].insert(userId);
    }
    qDebug() << "服务器启动：加载" << hashSet.size() << "个群组的成员缓存";
    return hashSet;
}

/**
 * @brief DataBaseMag::getGroupMemberDetail
 * 群成员详情：昵称、头像、角色、是否在线、入群时间。
 * 客户端"群成员"面板直接用这个数组渲染。
 */
QJsonValue DataBaseMag::getGroupMemberDetail(const int &groupId)
{
    QJsonArray jsonArr;

    QSqlQuery query(userdb);
    const QString sql =
            "SELECT gm.user_id, gm.identity, gm.join_time, "
            "       u.name, u.head, u.status "
            "FROM GROUP_MEMBER gm LEFT JOIN USERINFO u ON gm.user_id = u.id "
            "WHERE gm.group_id = :groupId "
            "ORDER BY gm.identity DESC, gm.join_time ASC;";
    query.prepare(sql);
    query.bindValue(":groupId", groupId);
    if (!query.exec()) {
        qDebug() << "[群成员] 查询失败：" << query.lastError().text();
        return QJsonValue(jsonArr);
    }

    while (query.next()) {
        QJsonObject obj;
        obj.insert("groupId", groupId);
        obj.insert("userId",  query.value("user_id").toInt());
        obj.insert("identity", query.value("identity").toInt());
        obj.insert("joinTime", query.value("join_time").toString());
        obj.insert("name",    query.value("name").toString());
        obj.insert("head",    query.value("head").toString());
        obj.insert("online",  query.value("status").toInt() == OnLine);
        jsonArr.append(obj);
    }
    return QJsonValue(jsonArr);
}

/**
 * @brief DataBaseMag::isGroupOwner
 * 群主判定以 GROUPS.creator_id 为准（identity 字段老数据可能没同步）
 */
bool DataBaseMag::isGroupOwner(const int &groupId, const int &userId)
{
    if (groupId <= 0 || userId <= 0) return false;

    QSqlQuery query(userdb);
    query.prepare("SELECT creator_id FROM GROUPS WHERE group_id = :gid LIMIT 1;");
    query.bindValue(":gid", groupId);
    if (query.exec() && query.next() && query.value("creator_id").toInt() == userId) {
        return true;
    }

    // 兜底：creator_id 没维护时看 GROUP_MEMBER.identity
    QSqlQuery q2(userdb);
    q2.prepare("SELECT identity FROM GROUP_MEMBER WHERE group_id = :gid AND user_id = :uid LIMIT 1;");
    q2.bindValue(":gid", groupId);
    q2.bindValue(":uid", userId);
    return q2.exec() && q2.next() && q2.value("identity").toInt() == Owner;
}

bool DataBaseMag::setGroupMemberIdentity(const int &groupId, const int &userId, const int &identity)
{
    QSqlQuery query(userdb);
    query.prepare("UPDATE GROUP_MEMBER SET identity = :identity "
                  "WHERE group_id = :gid AND user_id = :uid;");
    query.bindValue(":identity", identity);
    query.bindValue(":gid", groupId);
    query.bindValue(":uid", userId);
    if (!query.exec()) {
        qDebug() << "[群成员] 更新角色失败：" << query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool DataBaseMag::setGroupCreator(const int &groupId, const int &userId)
{
    QSqlQuery query(userdb);
    query.prepare("UPDATE GROUPS SET creator_id = :uid WHERE group_id = :gid;");
    query.bindValue(":uid", userId);
    query.bindValue(":gid", groupId);
    if (!query.exec()) {
        qDebug() << "[群成员] 更新群主失败：" << query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

QJsonValue DataBaseMag::getGroupMember(const int &groupId)
{
    QJsonArray jsonArr;
    QSqlQuery query;
    QString sql = "SELECT user_id, identity, join_time FROM GROUP_MEMBER WHERE group_id = :groupId";
    query.prepare(sql);
    query.bindValue(":groupId", groupId);
    if(!query.exec())
    {
        qDebug() << "获取群成员错误" << query.lastError();
        return QJsonValue(jsonArr);
    }
    while(query.next()) {
        QJsonObject jsonObj;
        jsonObj.insert("groupId", groupId);
        jsonObj.insert("userId", query.value("user_id").toInt());
        jsonObj.insert("identity", query.value("identity").toInt());
        jsonObj.insert("joinTime", query.value("join_time").toString());
        jsonArr.append(jsonObj);
    }
    return QJsonValue(jsonArr);
}

void DataBaseMag::queryAll()
{

}

// ==================================================================
// 端到端加密：用户公钥目录
// ==================================================================
/**
 * @brief 保存用户公钥
 *
 * 服务器只保存公钥这种本来就公开的材料。用户私钥永远只留在各自的机器上，
 * 服务器既没有托管副本，也没有管理员私钥，所以它在数学上无法解密任何正文。
 */
bool DataBaseMag::updateUserKeys(int id, const QString &pubKey)
{
    QSqlQuery query(userdb);
    query.prepare("UPDATE USERINFO SET pub_key = ? WHERE id = ?");
    query.addBindValue(pubKey);
    query.addBindValue(id);
    if (!query.exec()) {
        qWarning() << "[加密] 保存用户公钥失败：" << query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

QString DataBaseMag::getUserPubKey(int id)
{
    QSqlQuery query(userdb);
    query.prepare("SELECT pub_key FROM USERINFO WHERE id = ?");
    query.addBindValue(id);
    if (query.exec() && query.next()) return query.value(0).toString();
    return QString();
}

// ==================================================================
// 端到端加密：消息审计
// ==================================================================
void DataBaseMag::insertMsgLog(int fromId, int toId, bool isGroup, quint8 type, const QString &payload)
{
    QSqlQuery query(userdb);
    query.prepare("INSERT INTO MSGLOG (from_id, to_id, is_group, msg_type, payload) "
                  "VALUES (?, ?, ?, ?, ?)");
    query.addBindValue(fromId);
    query.addBindValue(toId);
    query.addBindValue(isGroup ? 1 : 0);
    query.addBindValue(static_cast<int>(type));
    query.addBindValue(payload);
    if (!query.exec()) {
        qWarning() << "[审计] 写入消息日志失败：" << query.lastError().text();
    }
}

QList<QVariantMap> DataBaseMag::getMsgLog(int limit)
{
    QList<QVariantMap> list;
    QSqlQuery query(userdb);
    query.prepare("SELECT m.id, m.send_time, m.from_id, m.to_id, m.is_group, m.msg_type, m.payload,"
                  "       uf.name AS from_name"
                  "  FROM MSGLOG m LEFT JOIN USERINFO uf ON uf.id = m.from_id"
                  " ORDER BY m.id DESC LIMIT ?");
    query.addBindValue(limit);
    if (!query.exec()) {
        qWarning() << "[审计] 读取消息日志失败：" << query.lastError().text();
        return list;
    }
    while (query.next()) {
        QVariantMap row;
        row.insert("id", query.value("id").toInt());
        row.insert("time", query.value("send_time").toString());
        row.insert("fromId", query.value("from_id").toInt());
        row.insert("toId", query.value("to_id").toInt());
        row.insert("isGroup", query.value("is_group").toInt() != 0);
        row.insert("type", query.value("msg_type").toInt());
        row.insert("payload", query.value("payload").toString());
        row.insert("fromName", query.value("from_name").toString());
        if (row.value("isGroup").toBool()) {
            row.insert("toName", getGroupName(query.value("to_id").toInt()));
        } else {
            row.insert("toName", getUsernameFromId(QString::number(query.value("to_id").toInt())));
        }
        list.append(row);
    }
    return list;
}

int DataBaseMag::getMsgLogCount()
{
    QSqlQuery query(userdb);
    if (query.exec("SELECT COUNT(*) FROM MSGLOG") && query.next())
        return query.value(0).toInt();
    return 0;
}

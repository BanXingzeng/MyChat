#include "databasemsg.h"
#include <QFileInfo>
#include "comapi/unit.h"
#include "comapi/myapp.h"
#include <QJsonArray>
#include <QVector>
#include <QDebug>
#include <QDateTime>
#include <QSqlQuery>
#include <QSqlError>
#include <algorithm>

DatabaseMsg *DatabaseMsg::self = nullptr;
DatabaseMsg::DatabaseMsg(QObject *parent) : QObject(parent)
{
}

DatabaseMsg::~DatabaseMsg()
{
    if (userdb.isOpen())
    {
        userdb.close();
    }

    if (msgdb.isOpen())
    {
        msgdb.close();
    }
}

bool DatabaseMsg::OpenUserDatabase(const QString &dataName)
{
    userdb = QSqlDatabase::addDatabase("QSQLITE", "connectionUser");
    userdb.setDatabaseName(dataName);
    if (!userdb.open())
    {
        qDebug() << "打开用户数据库失败";
        return false;
    }
    QSqlQuery query(userdb);
    QString sql;

    sql = QString("CREATE TABLE IF NOT EXISTS USERINFO (");
    sql.append(QString("id INTEGER PRIMARY KEY AUTOINCREMENT,"));
    sql.append(QString("name VARCHAR(30) NOT NULL UNIQUE,"));
    sql.append(QString("passwd VARCHAR(30) NOT NULL,"));
    sql.append(QString("head VARCHAR(100) DEFAULT 'default.png',"));
    sql.append(QString("status INTEGER DEFAULT %1,").arg(OffLine));
    sql.append(QString("lasttime DATETIME DEFAULT NULL);"));
    query.exec(sql);

    sql = QString("CREATE TABLE IF NOT EXISTS GROUPS (");
    sql.append(QString("group_id INTEGER PRIMARY KEY AUTOINCREMENT,"));
    sql.append(QString("group_name VARCHAR(50) NOT NULL,"));
    sql.append(QString("group_head VARCHAR(100) DEFAULT 'default_group.png',"));
    sql.append(QString("create_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"));
    sql.append(QString("creator_id INTEGER NOT NULL);"));
    query.exec(sql);

    sql = QString("CREATE TABLE IF NOT EXISTS GROUP_MEMBER (");
    sql.append(QString("id INTEGER PRIMARY KEY AUTOINCREMENT,"));
    sql.append(QString("group_id INTEGER NOT NULL,"));
    sql.append(QString("user_id INTEGER NOT NULL,"));
    sql.append(QString("identity INTEGER NOT NULL DEFAULT 0,"));
    sql.append(QString("join_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"));
    sql.append(QString("UNIQUE (group_id, user_id));"));
    query.exec(sql);

    sql = QString("CREATE TABLE IF NOT EXISTS FRIEND (");
    sql.append(QString("id INTEGER PRIMARY KEY AUTOINCREMENT,"));
    sql.append(QString("user_id INTEGER NOT NULL,"));          // 好友的用户ID
    sql.append(QString("friend_name VARCHAR(30) NOT NULL,"));  // 好友用户名
    sql.append(QString("remark VARCHAR(30) DEFAULT '',"));     // 好友备注
    sql.append(QString("status INTEGER NOT NULL DEFAULT 0,")); // 0=待验证/1=已通过/2=已拉黑
    sql.append(QString("head VARCHAR(100) DEFAULT 'default.png',")); // 好友头像文件名
    sql.append(QString("add_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"));
    sql.append(QString("update_time DATETIME DEFAULT CURRENT_TIMESTAMP,"));
    sql.append(QString("UNIQUE (user_id, friend_name));")); // 避免重复添加好友
    query.exec(sql);

    ensureFriendHeadColumn();
    return true;
}

bool DatabaseMsg::OpenMessageDatabase(const QString &dataName)
{
    // 避免重复连接
    if (QSqlDatabase::contains("connectionMsg"))
    {
        msgdb = QSqlDatabase::database("connectionMsg");
        if (msgdb.isOpen())
        {
            return true;
        }
    }

    msgdb = QSqlDatabase::addDatabase("QSQLITE", "connectionMsg");
    msgdb.setDatabaseName(dataName);

    if (!msgdb.open())
    {
        qDebug() << "打开消息数据库失败";
        return false;
    }

    QSqlQuery query(msgdb);
    QString sql;
    sql = QString("CREATE TABLE IF NOT EXISTS MSGINFO (");
    sql.append(QString("id INTEGER PRIMARY KEY AUTOINCREMENT,"));
    sql.append(QString("user_id INTEGER NOT NULL,")); // 好友的Id
    sql.append(QString("name VARCHAR(20) NOT NULL,"));
    sql.append(QString("head VARCHAR(100) DEFAULT 'default.png',"));
    sql.append(QString("datetime TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"));
    sql.append(QString("filesize VARCHAR(30) DEFAULT '',"));
    sql.append(QString("content VARCHAR(1000) NOT NULL,"));
    sql.append(QString("type INTEGER NOT NULL DEFAULT 0,"));
    sql.append(QString("direction INTEGER NOT NULL DEFAULT 0,"));
    sql.append(QString("color VARCHAR(20) DEFAULT '',"));
    sql.append(QString("msg_key VARCHAR(40) DEFAULT '',"));
    sql.append(QString("ts INTEGER NOT NULL DEFAULT 0,"));
    sql.append(QString("duration INTEGER NOT NULL DEFAULT 0,"));   // 语音时长(ms)
    sql.append(QString("unread INTEGER NOT NULL DEFAULT 0);"));   // 1=未读
    if (!query.exec(sql))
    {
        qDebug() << "【Msg数据库】创建MSGINFO表失败：" << query.lastError().text();
    }

    sql = QString("CREATE TABLE IF NOT EXISTS GROUPMSGINFO (");
    sql.append(QString("id INTEGER PRIMARY KEY AUTOINCREMENT,"));
    sql.append(QString("group_id INTEGER NOT NULL,"));
    sql.append(QString("user_id INTEGER NOT NULL,")); // 发消息用户的Id
    sql.append(QString("name VARCHAR(20) NOT NULL,"));
    sql.append(QString("head VARCHAR(100) DEFAULT 'default.png',"));
    sql.append(QString("datetime TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"));
    sql.append(QString("filesize VARCHAR(30) DEFAULT '',"));
    sql.append(QString("content VARCHAR(1000) NOT NULL,"));
    sql.append(QString("type INTEGER NOT NULL DEFAULT 0,"));
    sql.append(QString("direction INTEGER NOT NULL DEFAULT 0,"));
    sql.append(QString("color VARCHAR(20) DEFAULT '',"));
    sql.append(QString("msg_key VARCHAR(40) DEFAULT '',"));
    sql.append(QString("ts INTEGER NOT NULL DEFAULT 0,"));
    sql.append(QString("duration INTEGER NOT NULL DEFAULT 0,"));   // 语音时长(ms)
    sql.append(QString("unread INTEGER NOT NULL DEFAULT 0);"));   // 1=未读
    if (!query.exec(sql))
    {
        qDebug() << "【Msg数据库】创建GROUPMSGINFO表失败：" << query.lastError().text();
    }

    // 老版本的消息库没有 unread 字段，这里补上，否则离线未读统计无法持久化
    ensureUnreadColumn("MSGINFO");
    ensureUnreadColumn("GROUPMSGINFO");

    // 文字消息的自定义颜色
    ensureColorColumn("MSGINFO");
    ensureColorColumn("GROUPMSGINFO");

    // 历史脏数据：图片消息曾被当作文字入库（type=0），这里按内容前缀纠正
    fixPictureMsgType("MSGINFO");
    fixPictureMsgType("GROUPMSGINFO");

    // 撤回 / 删除需要的消息唯一标识与时间戳
    ensureMsgKeyColumn("MSGINFO");
    ensureMsgKeyColumn("GROUPMSGINFO");

    // 语音消息时长（毫秒）
    ensureDurationColumn("MSGINFO");
    ensureDurationColumn("GROUPMSGINFO");
    return true;
}

/**
 * @brief DatabaseMsg::ensureMsgKeyColumn
 * 为老数据库补齐 msg_key / ts 两个字段（撤回要按 key 定位、按 ts 判断是否超时）
 */
void DatabaseMsg::ensureMsgKeyColumn(const QString &table)
{
    QSqlQuery info(msgdb);
    if (!info.exec(QString("PRAGMA table_info(%1);").arg(table)))
    {
        qDebug() << "查询表结构失败" << table << info.lastError().text();
        return;
    }

    bool hasKey = false;
    bool hasTs = false;
    while (info.next())
    {
        const QString name = info.value(1).toString();
        if (0 == name.compare("msg_key", Qt::CaseInsensitive)) hasKey = true;
        if (0 == name.compare("ts", Qt::CaseInsensitive))      hasTs = true;
    }

    if (!hasKey)
    {
        QSqlQuery alter(msgdb);
        if (!alter.exec(QString("ALTER TABLE %1 ADD COLUMN msg_key VARCHAR(40) DEFAULT '';").arg(table)))
        {
            qDebug() << "为" << table << "添加 msg_key 字段失败：" << alter.lastError().text();
        }
        else
        {
            qDebug() << "为" << table << "添加 msg_key 字段成功";
        }
    }

    if (!hasTs)
    {
        QSqlQuery alter(msgdb);
        if (!alter.exec(QString("ALTER TABLE %1 ADD COLUMN ts INTEGER NOT NULL DEFAULT 0;").arg(table)))
        {
            qDebug() << "为" << table << "添加 ts 字段失败：" << alter.lastError().text();
        }
        else
        {
            qDebug() << "为" << table << "添加 ts 字段成功";
        }
    }

    // 历史消息没有 msg_key：按行号补一个（m5 / g5），否则右键"删除"找不到这条
    // 注意 ts 保持 0 —— 老消息不该还能撤回
    QSqlQuery fill(msgdb);
    const QString prefix = (table == "MSGINFO") ? "m" : "g";
    if (!fill.exec(QString("UPDATE %1 SET msg_key = '%2' || id "
                           "WHERE msg_key IS NULL OR msg_key = '';").arg(table, prefix)))
    {
        qDebug() << "为" << table << "补 msg_key 失败：" << fill.lastError().text();
    }
    else if (fill.numRowsAffected() > 0)
    {
        qDebug() << "为" << table << "补 msg_key" << fill.numRowsAffected() << "条";
    }
}

/**
 * @brief DatabaseMsg::ensureDurationColumn
 * 为老数据库补齐 duration 字段（语音消息时长，毫秒）。
 * SQLite 不支持 ADD COLUMN IF NOT EXISTS，先查表结构再决定要不要加。
 */
void DatabaseMsg::ensureDurationColumn(const QString &table)
{
    QSqlQuery info(msgdb);
    if (!info.exec(QString("PRAGMA table_info(%1);").arg(table)))
    {
        qDebug() << "查询表结构失败" << table << info.lastError().text();
        return;
    }

    while (info.next())
    {
        if (0 == info.value(1).toString().compare("duration", Qt::CaseInsensitive))
        {
            return;     // 已经有了
        }
    }

    QSqlQuery alter(msgdb);
    if (!alter.exec(QString("ALTER TABLE %1 ADD COLUMN duration INTEGER NOT NULL DEFAULT 0;").arg(table)))
    {
        qDebug() << "为" << table << "添加 duration 字段失败：" << alter.lastError().text();
        return;
    }
    qDebug() << "为" << table << "添加 duration 字段成功";
}

/**
 * @brief DatabaseMsg::deleteMsgByKey
 * 按消息唯一标识删除一条消息（撤回 / 删除共用），两个表都试一遍
 */
bool DatabaseMsg::deleteMsgByKey(const QString &msgKey)
{
    if (msgKey.isEmpty()) return false;

    const QStringList tables = QStringList() << "MSGINFO" << "GROUPMSGINFO";
    for (const QString &table : tables)
    {
        QSqlQuery del(msgdb);
        del.prepare(QString("DELETE FROM %1 WHERE msg_key = :key;").arg(table));
        del.bindValue(":key", msgKey);
        if (!del.exec())
        {
            qDebug() << "删除消息失败" << table << del.lastError().text();
            continue;
        }
        if (del.numRowsAffected() > 0)
        {
            qDebug() << "[消息] 已从" << table << "删除 key =" << msgKey;
            return true;
        }
    }
    return false;
}

/**
 * @brief DatabaseMsg::ensureUnreadColumn
 * 为老数据库补齐 unread 字段（SQLite 不支持 IF NOT EXISTS 添加列，先查表结构）
 */
void DatabaseMsg::ensureUnreadColumn(const QString &table)
{
    QSqlQuery info(msgdb);
    if (!info.exec(QString("PRAGMA table_info(%1);").arg(table)))
    {
        qDebug() << "查询表结构失败" << table << info.lastError().text();
        return;
    }

    bool hasColumn = false;
    while (info.next())
    {
        if (0 == info.value(1).toString().compare("unread", Qt::CaseInsensitive))
        {
            hasColumn = true;
            break;
        }
    }
    if (hasColumn) return;

    QSqlQuery alter(msgdb);
    if (!alter.exec(QString("ALTER TABLE %1 ADD COLUMN unread INTEGER NOT NULL DEFAULT 0;").arg(table)))
    {
        qDebug() << "为" << table << "添加 unread 字段失败：" << alter.lastError().text();
        return;
    }
    qDebug() << "为" << table << "添加 unread 字段成功";
}

/**
 * @brief DatabaseMsg::fixPictureMsgType
 * 早期版本私聊转发丢了 type 字段，图片消息按 Text(0) 入库，气泡就把 base64 当文字显示。
 * 这里把"内容是 b64: 开头但类型还是 0"的行纠正为 Picture(2)。
 */
void DatabaseMsg::fixPictureMsgType(const QString &table)
{
    QSqlQuery fix(msgdb);
    const QString sql = QString("UPDATE %1 SET type = 2 "
                                "WHERE type = 0 AND content LIKE 'b64:%';").arg(table);
    if (!fix.exec(sql))
    {
        qDebug() << "纠正" << table << "图片消息类型失败：" << fix.lastError().text();
        return;
    }
    if (fix.numRowsAffected() > 0)
    {
        qDebug() << "纠正" << table << "图片消息类型" << fix.numRowsAffected() << "条";
    }
}

/**
 * @brief DatabaseMsg::ensureColorColumn
 * 为老数据库补齐 color 字段（文字消息的自定义颜色）
 */
void DatabaseMsg::ensureColorColumn(const QString &table)
{
    QSqlQuery info(msgdb);
    if (!info.exec(QString("PRAGMA table_info(%1);").arg(table)))
    {
        qDebug() << "查询表结构失败" << table << info.lastError().text();
        return;
    }

    bool hasColumn = false;
    while (info.next())
    {
        if (0 == info.value(1).toString().compare("color", Qt::CaseInsensitive))
        {
            hasColumn = true;
            break;
        }
    }
    if (hasColumn) return;

    QSqlQuery alter(msgdb);
    if (!alter.exec(QString("ALTER TABLE %1 ADD COLUMN color VARCHAR(20) DEFAULT '';").arg(table)))
    {
        qDebug() << "为" << table << "添加 color 字段失败：" << alter.lastError().text();
        return;
    }
    qDebug() << "为" << table << "添加 color 字段成功";
}

/**
 * @brief DatabaseMsg::ensureFriendHeadColumn
 * 老数据库的 FRIEND 表没有 head 字段，这里补上
 */
void DatabaseMsg::ensureFriendHeadColumn()
{
    QSqlQuery info(userdb);
    if (!info.exec("PRAGMA table_info(FRIEND);"))
    {
        qDebug() << "查询 FRIEND 表结构失败" << info.lastError().text();
        return;
    }

    bool hasColumn = false;
    while (info.next())
    {
        if (0 == info.value(1).toString().compare("head", Qt::CaseInsensitive))
        {
            hasColumn = true;
            break;
        }
    }
    if (hasColumn) return;

    QSqlQuery alter(userdb);
    if (!alter.exec("ALTER TABLE FRIEND ADD COLUMN head VARCHAR(100) DEFAULT 'default.png';"))
    {
        qDebug() << "为 FRIEND 添加 head 字段失败：" << alter.lastError().text();
        return;
    }
    qDebug() << "为 FRIEND 添加 head 字段成功";
}

/**
 * @brief DatabaseMsg::getFriendHead
 * 取好友当前的头像文件名，没有记录返回空
 */
QString DatabaseMsg::getFriendHead(const int &id)
{
    if (id < 0) return QString();

    QSqlQuery query(userdb);
    query.prepare("SELECT head FROM FRIEND WHERE user_id = :id;");
    query.bindValue(":id", id);
    if (!query.exec() || !query.next())
    {
        return QString();
    }
    return query.value("head").toString();
}

/**
 * @brief DatabaseMsg::updateFriendHead
 * 好友换了头像：更新好友表，同时把历史消息里该好友发的消息头像一起改过来，
 * 这样重开聊天窗口、会话列表都不会还挂着旧头像
 */
void DatabaseMsg::updateFriendHead(const int &friendId, const QString &head)
{
    if (friendId < 0 || head.isEmpty()) return;

    QSqlQuery query(userdb);
    query.prepare("UPDATE FRIEND SET head = :head, update_time = CURRENT_TIMESTAMP WHERE user_id = :id;");
    query.bindValue(":head", head);
    query.bindValue(":id", friendId);
    if (!query.exec())
    {
        qDebug() << "更新好友头像失败" << query.lastError().text();
    }

    if (msgdb.isOpen())
    {
        QSqlQuery msgQuery(msgdb);
        msgQuery.prepare("UPDATE MSGINFO SET head = :head WHERE user_id = :id AND direction = 1;");
        msgQuery.bindValue(":head", head);
        msgQuery.bindValue(":id", friendId);
        msgQuery.exec();
    }
}

QString DatabaseMsg::getFriendName(const int &id)
{
    QString userName = "";
    if (id < 0)
    {
        qDebug() << "获取好友名失败，id < 0";
        return userName;
    }
    QSqlQuery query(userdb);
    QString sql("SELECT friend_name FROM FRIEND where user_id = :id;");
    query.prepare(sql);
    query.bindValue(":id", id);
    if (!query.exec())
    {
        qDebug() << "获取用户名失败" << query.lastError();
        return userName;
    }
    if (!query.next())
    {
        return userName;    // 好友表里没有这条记录，返回空字符串
    }
    userName = query.value("friend_name").toString();
    return userName;
}

void DatabaseMsg::AddFriend(const int &userId, const QString &friendName, int status)
{
    bool isFriend = isMyFriend(friendName);
    if (!isFriend)
    { // 不是好友
        QSqlQuery query(userdb);
        QString sql;
        sql = QString("INSERT INTO FRIEND (");
        sql.append("user_id, friend_name, remark, status, add_time, update_time) ");
        sql.append("VALUES (:userId, :friendName, :remark, :status, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)");

        query.prepare(sql);
        query.bindValue(":userId", userId);
        query.bindValue(":friendName", friendName);
        query.bindValue(":remark", friendName);
        query.bindValue(":status", status);
        if (!query.exec())
        {
            qDebug() << "数据库插入好友失败" << query.lastError();
        }
    }
    // 好友表已经有这条记录，收到同意消息修改status
    QSqlQuery query(userdb);
    QString sql;
    sql = QString("UPDATE FRIEND SET status = :status, update_time = CURRENT_TIMESTAMP");
    sql.append("WHERE user_id = :userId AND friend_name = :friendName;");
    query.prepare(sql);
    query.bindValue(":status", status);
    query.bindValue(":userId", userId);
    query.bindValue(":friendName", friendName);
}

QJsonValue DatabaseMsg::GetMyFriend()
{
    QJsonArray jsonArr;
    QSqlQuery query(userdb);
    QString sql;
    sql = QString("SELECT user_id FROM FRIEND;");

    if (!query.exec(sql))
    {
        qDebug() << "查询我的好友Id失败" << query.lastError();
        return QJsonValue();
    }
    while (query.next())
    {
        QJsonObject jsonObj;
        jsonObj.insert("id", query.value("user_id").toInt());
        jsonArr.append(jsonObj);
    }
    QJsonValue jsonVal(jsonArr);
    return jsonVal;
}

bool DatabaseMsg::deleteFriend(const int &userId)
{
    if(userId < 0){
        qDebug() << "删除好友失败，好友Id < 0";
        return false;
    }
    QSqlQuery query(userdb);
    QString sql = QString("DELETE FROM FRIEND WHERE user_id = :userId;");
    query.prepare(sql);
    query.bindValue(":userId", userId);
    if(!query.exec()){
        qDebug() << "删除好友操作错误" << query.lastError();
        return false;
    }
    return true;
}

void DatabaseMsg::AddHistoryMsg(const int &userId, ItemInfo *itemInfo)
{
    if (userId < 0)
    {
        qDebug() << "添加历史消息出错userId < 0";
        return;
    }
    QSqlQuery query(msgdb);
    QString sql;
    sql = QString("INSERT INTO MSGINFO (user_id, name, head, datetime, filesize, content, type, direction, color, unread, msg_key, ts, duration) ");
    sql.append("VALUES (:user_id, :name, :head, :datetime, :filesize, :content, :type, :direction, :color, :unread, :msg_key, :ts, :duration);");
    query.prepare(sql);
    query.bindValue(":user_id", userId);
    query.bindValue(":name", itemInfo->GetName());
    query.bindValue(":head", QFileInfo(itemInfo->GetStrPixmap()).fileName());
    query.bindValue(":datetime", itemInfo->GetDatetime());
    query.bindValue(":filesize", itemInfo->GetFileSizeString());
    if (itemInfo->GetMsgType() == Text || itemInfo->GetMsgType() == Files
            || itemInfo->GetMsgType() == Picture || itemInfo->GetMsgType() == Audio)
    {
        query.bindValue(":content", itemInfo->GetText());
    }
    else if (itemInfo->GetMsgType() == Face)
    {
        query.bindValue(":content", QString::number(itemInfo->getFace()));
    }
    else
    {
        // 保证 content 字段不会为 NULL
        query.bindValue(":content", QString(""));
    }
    query.bindValue(":type", static_cast<int>(itemInfo->GetMsgType()));
    query.bindValue(":direction", itemInfo->GetOrientation());
    query.bindValue(":color", itemInfo->GetTextColor());
    // 只有收到别人发的消息才算未读，自己发出去的永远是已读
    query.bindValue(":unread", itemInfo->GetOrientation() == Left ? 1 : 0);
    query.bindValue(":msg_key", itemInfo->GetMsgKey());
    // 没带时间戳的老消息按当前时间算，保证"刚发的"能撤回
    query.bindValue(":ts", itemInfo->GetTimeStamp() > 0 ? itemInfo->GetTimeStamp()
                                                        : QDateTime::currentMSecsSinceEpoch());
    query.bindValue(":duration", itemInfo->GetDuration());

    if (!query.exec())
    {
        qDebug() << "插入历史消息出错" << query.lastError();
    }
    //    qDebug() << "插入到数据库消息：" << "content" << itemInfo->getFace() << "type" << itemInfo->GetMsgType();
}

void DatabaseMsg::AddGroupHistoryMsg(const int &userId, const int &groupId, ItemInfo *itemInfo)
{
    if (userId < 0 || groupId < 0)
    {
        qDebug() << "添加群组历史消息出错userId < 0或groupId < 0";
        return;
    }
    QSqlQuery query(msgdb);
    QString sql;
    sql = QString("INSERT INTO GROUPMSGINFO (group_id, user_id, name, head, datetime, filesize, content, type, direction, color, unread, msg_key, ts, duration) ");
    sql.append("VALUES (:groupId, :userId, :name, :head, :datetime, :filesize, :content, :type, :direction, :color, :unread, :msg_key, :ts, :duration);");
    query.prepare(sql);
    query.bindValue(":groupId", groupId);
    query.bindValue(":userId", userId);
    query.bindValue(":name", itemInfo->GetName());
    query.bindValue(":head", QFileInfo(itemInfo->GetStrPixmap()).fileName());
    query.bindValue(":datetime", itemInfo->GetDatetime());
    query.bindValue(":filesize", itemInfo->GetFileSizeString());
    if (itemInfo->GetMsgType() == Text || itemInfo->GetMsgType() == Files
            || itemInfo->GetMsgType() == Picture || itemInfo->GetMsgType() == Audio)
    {
        query.bindValue(":content", itemInfo->GetText());
    }
    else if (itemInfo->GetMsgType() == Face)
    {
        query.bindValue(":content", QString::number(itemInfo->getFace()));
    }
    else
    {
        // 保证 content 字段不会为 NULL
        query.bindValue(":content", QString(""));
    }
    query.bindValue(":type", static_cast<int>(itemInfo->GetMsgType()));
    query.bindValue(":direction", itemInfo->GetOrientation());
    query.bindValue(":color", itemInfo->GetTextColor());
    // 只有收到别人发的消息才算未读，自己发出去的永远是已读
    query.bindValue(":unread", itemInfo->GetOrientation() == Left ? 1 : 0);
    query.bindValue(":msg_key", itemInfo->GetMsgKey());
    // 没带时间戳的老消息按当前时间算，保证"刚发的"能撤回
    query.bindValue(":ts", itemInfo->GetTimeStamp() > 0 ? itemInfo->GetTimeStamp()
                                                        : QDateTime::currentMSecsSinceEpoch());
    query.bindValue(":duration", itemInfo->GetDuration());

    if (!query.exec())
    {
        qDebug() << "插入历史消息出错" << query.lastError();
    }
}

QVector<QJsonObject> DatabaseMsg::getHistoryMsg(const int &id, const int &count)
{
    const int maxCount = 10;
    int n = 0;
    qDebug() << "count = " << count;
    QVector<QJsonObject> vJsonObj;
    QSqlQuery query(msgdb);
    QString sql;
    sql = QString("SELECT name, head, datetime, filesize, content, type, direction, color, msg_key, ts, duration FROM MSGINFO WHERE user_id = :id");
    sql.append(" ORDER BY id DESC LIMIT 10 OFFSET :count");
    query.prepare(sql);
    query.bindValue(":id", id);
    query.bindValue(":count", count * maxCount);
    if (!query.exec())
    {
        qDebug() << "查询用户历史信息失败" << query.lastError();
        return vJsonObj;
    }
    while (query.next() && n++ < maxCount)
    {
        QJsonObject Obj;
        Obj.insert("name", query.value("name").toString());
        Obj.insert("head", query.value("head").toString());
        Obj.insert("datetime", query.value("datetime").toString());
        Obj.insert("filesize", query.value("filesize").toString());
        Obj.insert("content", query.value("content").toString());
        Obj.insert("type", query.value("type").toInt());
        Obj.insert("direction", query.value("direction").toInt());
        Obj.insert("color", query.value("color").toString());
        Obj.insert("msg_key", query.value("msg_key").toString());
        Obj.insert("ts", query.value("ts").toLongLong());
        Obj.insert("duration", query.value("duration").toInt());
        vJsonObj.append(Obj);
        //        qDebug() << Obj.value("content");
    }
    return vJsonObj;
}

QVector<QJsonObject> DatabaseMsg::getGroupHistoryMsg(const int &groupId, const int &count)
{
    const int maxCount = 10;
    int n = 0;
    qDebug() << "count = " << count;
    QVector<QJsonObject> vJsonObj;
    QSqlQuery query(msgdb);
    QString sql;
    sql = QString("SELECT user_id, name, head, datetime, filesize, content, type, direction, color, msg_key, ts, duration FROM GROUPMSGINFO WHERE group_id = :groupId");
    sql.append(" ORDER BY id DESC LIMIT 10 OFFSET :count");
    query.prepare(sql);
    query.bindValue(":groupId", groupId);
    query.bindValue(":count", count * maxCount);
    if (!query.exec())
    {
        qDebug() << "查询用户历史信息失败" << query.lastError();
        return vJsonObj;
    }
    while (query.next() && n++ < maxCount)
    {
        QJsonObject Obj;
        Obj.insert("user_id", query.value("user_id").toInt());
        Obj.insert("name", query.value("name").toString());
        Obj.insert("head", query.value("head").toString());
        Obj.insert("datetime", query.value("datetime").toString());
        Obj.insert("filesize", query.value("filesize").toString());
        Obj.insert("content", query.value("content").toString());
        Obj.insert("type", query.value("type").toInt());
        Obj.insert("direction", query.value("direction").toInt());
        Obj.insert("color", query.value("color").toString());
        Obj.insert("msg_key", query.value("msg_key").toString());
        Obj.insert("ts", query.value("ts").toLongLong());
        Obj.insert("duration", query.value("duration").toInt());
        vJsonObj.append(Obj);
        //        qDebug() << Obj.value("content");
    }
    return vJsonObj;
}

/**
 * @brief DatabaseMsg::getAllHistoryMsg
 * 取与某个好友的全部聊天记录（正序），供聊天记录窗口使用
 */
/**
 * @brief DatabaseMsg::searchAllMessages
 * 跨会话搜索聊天记录：私聊表和群聊表各查一次，合并后按时间倒序。
 *
 * 注意：客户端的用户名/群名存在 userdb（userN.db），消息存在 msgdb（msgN.db），
 * 是两个独立的库文件，不能 JOIN，所以名字在拿到消息后单独补一次（带缓存）。
 *
 * 返回结构统一为：
 *   { sessionId, sessionName, isGroup, sender, head, datetime, content, type, direction, ts }
 */
QVector<QJsonObject> DatabaseMsg::searchAllMessages(const QString &keyword, int limit)
{
    QVector<QJsonObject> result;
    const QString kw = keyword.trimmed();
    if (kw.isEmpty() || !msgdb.isOpen()) return result;

    // LIKE 的通配符要转义，否则用户输入 % 会把整库都搜出来
    QString pattern = kw;
    pattern.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_");
    pattern = "%" + pattern + "%";

    if (limit <= 0) limit = 300;

    // 会话名缓存：一次搜索里同一个会话只查一次库
    QHash<int, QString> friendNameCache;
    QHash<int, QString> groupNameCache;

    auto friendNameOf = [this, &friendNameCache](int friendId) -> QString {
        if (friendNameCache.contains(friendId)) return friendNameCache.value(friendId);
        QString name;
        QSqlQuery q(userdb);
        q.prepare("SELECT friend_name FROM FRIEND WHERE user_id = :me AND friend_id = :fid LIMIT 1;");
        q.bindValue(":me", MyApp::m_nId);
        q.bindValue(":fid", friendId);
        if (q.exec() && q.next()) name = q.value("friend_name").toString();
        friendNameCache.insert(friendId, name);
        return name;
    };
    auto groupNameOf = [this, &groupNameCache](int groupId) -> QString {
        if (groupNameCache.contains(groupId)) return groupNameCache.value(groupId);
        const QString name = getGroupName(groupId);
        groupNameCache.insert(groupId, name);
        return name;
    };

    // ---- 私聊：会话对方 id 就是 MSGINFO.user_id ----
    {
        QSqlQuery query(msgdb);
        query.prepare("SELECT user_id, name, head, datetime, content, type, direction, ts "
                      "FROM MSGINFO "
                      "WHERE content LIKE :kw ESCAPE '\\' AND content NOT LIKE 'b64:%' "
                      "ORDER BY ts DESC, id DESC LIMIT :limit;");
        query.bindValue(":kw", pattern);
        query.bindValue(":limit", limit);
        if (query.exec()) {
            while (query.next()) {
                const int sessionId = query.value("user_id").toInt();
                QJsonObject obj;
                obj.insert("sessionId", sessionId);
                obj.insert("sessionName", friendNameOf(sessionId));
                obj.insert("isGroup", false);
                obj.insert("sender", query.value("name").toString());
                obj.insert("head", query.value("head").toString());
                obj.insert("datetime", query.value("datetime").toString());
                obj.insert("content", query.value("content").toString());
                obj.insert("type", query.value("type").toInt());
                obj.insert("direction", query.value("direction").toInt());
                obj.insert("ts", query.value("ts").toLongLong());
                result.append(obj);
            }
        } else {
            qDebug() << "[搜索] 私聊记录查询失败：" << query.lastError().text();
        }
    }

    // ---- 群聊 ----
    {
        QSqlQuery query(msgdb);
        query.prepare("SELECT group_id, name, head, datetime, content, type, direction, ts "
                      "FROM GROUPMSGINFO "
                      "WHERE content LIKE :kw ESCAPE '\\' AND content NOT LIKE 'b64:%' "
                      "ORDER BY ts DESC, id DESC LIMIT :limit;");
        query.bindValue(":kw", pattern);
        query.bindValue(":limit", limit);
        if (query.exec()) {
            while (query.next()) {
                const int sessionId = query.value("group_id").toInt();
                QJsonObject obj;
                obj.insert("sessionId", sessionId);
                obj.insert("sessionName", groupNameOf(sessionId));
                obj.insert("isGroup", true);
                obj.insert("sender", query.value("name").toString());
                obj.insert("head", query.value("head").toString());
                obj.insert("datetime", query.value("datetime").toString());
                obj.insert("content", query.value("content").toString());
                obj.insert("type", query.value("type").toInt());
                obj.insert("direction", query.value("direction").toInt());
                obj.insert("ts", query.value("ts").toLongLong());
                result.append(obj);
            }
        } else {
            qDebug() << "[搜索] 群聊记录查询失败：" << query.lastError().text();
        }
    }

    // 两张表各自有序，合并后整体再按时间倒序
    std::sort(result.begin(), result.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const qint64 ta = a.value("ts").toVariant().toLongLong();
        const qint64 tb = b.value("ts").toVariant().toLongLong();
        return ta > tb;
    });

    if (result.size() > limit) result.resize(limit);
    return result;
}

QVector<QJsonObject> DatabaseMsg::getAllHistoryMsg(const int &id)
{
    QVector<QJsonObject> vJsonObj;
    QSqlQuery query(msgdb);
    query.prepare("SELECT name, head, datetime, filesize, content, type, direction, color, msg_key, ts, duration "
                  "FROM MSGINFO WHERE user_id = :id ORDER BY id ASC");
    query.bindValue(":id", id);
    if (!query.exec())
    {
        qDebug() << "查询全部历史信息失败" << query.lastError();
        return vJsonObj;
    }
    while (query.next())
    {
        QJsonObject Obj;
        Obj.insert("name", query.value("name").toString());
        Obj.insert("head", query.value("head").toString());
        Obj.insert("datetime", query.value("datetime").toString());
        Obj.insert("filesize", query.value("filesize").toString());
        Obj.insert("content", query.value("content").toString());
        Obj.insert("type", query.value("type").toInt());
        Obj.insert("direction", query.value("direction").toInt());
        Obj.insert("color", query.value("color").toString());
        Obj.insert("msg_key", query.value("msg_key").toString());
        Obj.insert("ts", query.value("ts").toLongLong());
        Obj.insert("duration", query.value("duration").toInt());
        vJsonObj.append(Obj);
    }
    return vJsonObj;
}

/**
 * @brief DatabaseMsg::getAllGroupHistoryMsg
 * 取某个群的全部聊天记录（正序），供聊天记录窗口使用
 */
QVector<QJsonObject> DatabaseMsg::getAllGroupHistoryMsg(const int &groupId)
{
    QVector<QJsonObject> vJsonObj;
    QSqlQuery query(msgdb);
    query.prepare("SELECT user_id, name, head, datetime, filesize, content, type, direction, color, msg_key, ts, duration "
                  "FROM GROUPMSGINFO WHERE group_id = :groupId ORDER BY id ASC");
    query.bindValue(":groupId", groupId);
    if (!query.exec())
    {
        qDebug() << "查询群组全部历史信息失败" << query.lastError();
        return vJsonObj;
    }
    while (query.next())
    {
        QJsonObject Obj;
        Obj.insert("user_id", query.value("user_id").toInt());
        Obj.insert("name", query.value("name").toString());
        Obj.insert("head", query.value("head").toString());
        Obj.insert("datetime", query.value("datetime").toString());
        Obj.insert("filesize", query.value("filesize").toString());
        Obj.insert("content", query.value("content").toString());
        Obj.insert("type", query.value("type").toInt());
        Obj.insert("direction", query.value("direction").toInt());
        Obj.insert("color", query.value("color").toString());
        Obj.insert("msg_key", query.value("msg_key").toString());
        Obj.insert("ts", query.value("ts").toLongLong());
        Obj.insert("duration", query.value("duration").toInt());
        vJsonObj.append(Obj);
    }
    return vJsonObj;
}

bool DatabaseMsg::isMyFriend(QString friendName){

    QSqlQuery query(userdb);
    QString sql = "SELECT friend_name FROM FRIEND WHERE friend_name = :friendName LIMIT 1";

    query.prepare(sql);
    query.bindValue(":friendName", friendName);
    query.exec();

    if (!query.next())
    {
        qDebug() << "查询是不是好友失败" << query.lastError();
        return false; // 不是好友
    }
    return true; // 是好友

    //    if (query.next()){
    //        status = query.value("status").toInt();
    //        if(status == 0){
    //            return AddFriendFailed_Readd;   //添加申请还未通过
    //        }else if(status == 1 || status == 2){
    //            return  AddFriendFailed_IsHad;  //已经是好友了
    //        }
    //    }
    //    return AddFriendOk; //不是好友
    //    qDebug() << "status = " << status;
}

void DatabaseMsg::removeFriend(const QString &friendName)
{
    if (friendName.isEmpty())
    {
        qDebug() << "删除好友失败，用户名不能为空";
        return;
    }
    QSqlQuery query(userdb);
    QString sql;
    sql = QString("DELETE FROM FRIEND WHERE friend_name = :friendId");
    query.prepare(sql);
    query.bindValue(":friendId", friendName);
    if (!query.exec())
    {
        qDebug() << "删除好友操作Sql执行失败";
        return;
    }
}

void DatabaseMsg::AddGroup(const int &userId, const QString &groupHead, const QString &groupName, const int &groupId)
{
    qDebug() << "添加群组" << userId << groupHead << groupName << groupId << "\n\n";
    if(userId < 0 || groupName.isEmpty()){
        qDebug() << "数据库插入群组失败";
        return;
    }
    QSqlQuery query(userdb);
    QString sql;
    sql = QString("INSERT INTO GROUPS (group_id, group_head, group_name, creator_id) ");
    sql.append("VALUES (:groupId, :groupHead, :groupName, :creatorId);");
    query.prepare(sql);
    query.bindValue(":groupId", groupId);
    query.bindValue(":groupHead", groupHead);
    query.bindValue(":groupName", groupName);
    query.bindValue(":creatorId", userId);
    if(!query.exec()){
        qDebug() << "添加群组出错" << query.lastError();
        return ;
    }

}

QString DatabaseMsg::getGroupName(const int &groupId)
{
    QSqlQuery query(userdb);
    QString sql;
    sql = QString("SELECT group_name FROM GROUPS WHERE group_id = :groupId;");
    query.prepare(sql);
    query.bindValue(":groupId", groupId);
    if(!query.exec()){
        qDebug() << "获取群组名出错" << query.lastError();
        return "";
    }
    if(!query.next()){
        return "";      // 群组表里没有这条记录
    }
    return query.value("group_name").toString();
}

bool DatabaseMsg::addGroupMember(const int &userId, const int &groupId, GroupIdentity identity)
{
    if(userId < 0 || groupId < 0) return false;
    QSqlQuery query(userdb);
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

void DatabaseMsg::addGroupMember(const int &userId, const int &groupId, const QString &joinTime, GroupIdentity identity)
{
    if(userId < 0 || groupId < 0) return;
    QSqlQuery query(userdb);
    QString sql = "INSERT INTO GROUP_MEMBER (group_id, user_id, identity, join_time) VALUES (:groupId, :userId, :joinTime, :identity);";
    query.prepare(sql);
    query.bindValue(":groupId", groupId);
    query.bindValue(":userId", userId);
    query.bindValue(":joinTime", joinTime);
    query.bindValue(":identity", identity);
    if(!query.exec()){
        qDebug() << "添加群成员错误" << query.lastError();
        return;
    }
}

QString DatabaseMsg::getGroupHead(const int &groupId)
{
    QSqlQuery query(userdb);
    query.prepare("SELECT group_head FROM GROUPS WHERE group_id = :groupId;");
    query.bindValue(":groupId", groupId);
    if (!query.exec() || !query.next())
    {
        return QString();
    }
    return query.value("group_head").toString();
}

///////////////////////////////////////////////////////////////////////////
/// 未读消息统计
///

int DatabaseMsg::getUnreadCountByFriend(const int &friendId)
{
    QSqlQuery query(msgdb);
    query.prepare("SELECT COUNT(*) FROM MSGINFO WHERE user_id = :id AND unread = 1;");
    query.bindValue(":id", friendId);
    if (!query.exec() || !query.next())
    {
        qDebug() << "统计好友未读数失败" << query.lastError().text();
        return 0;
    }
    return query.value(0).toInt();
}

int DatabaseMsg::getUnreadCountByGroup(const int &groupId)
{
    QSqlQuery query(msgdb);
    query.prepare("SELECT COUNT(*) FROM GROUPMSGINFO WHERE group_id = :id AND unread = 1;");
    query.bindValue(":id", groupId);
    if (!query.exec() || !query.next())
    {
        qDebug() << "统计群组未读数失败" << query.lastError().text();
        return 0;
    }
    return query.value(0).toInt();
}

int DatabaseMsg::getUnreadTotal(const bool &isGroup)
{
    const QString sql = isGroup
            ? "SELECT COUNT(*) FROM GROUPMSGINFO WHERE unread = 1;"
            : "SELECT COUNT(*) FROM MSGINFO WHERE unread = 1;";
    QSqlQuery query(msgdb);
    if (!query.exec(sql) || !query.next())
    {
        qDebug() << "统计未读总数失败" << query.lastError().text();
        return 0;
    }
    return query.value(0).toInt();
}

void DatabaseMsg::markSessionRead(const int &id, const bool &isGroup)
{
    if (id < 0)
    {
        return;
    }
    QSqlQuery query(msgdb);
    if (isGroup)
    {
        query.prepare("UPDATE GROUPMSGINFO SET unread = 0 WHERE group_id = :id AND unread = 1;");
    }
    else
    {
        query.prepare("UPDATE MSGINFO SET unread = 0 WHERE user_id = :id AND unread = 1;");
    }
    query.bindValue(":id", id);
    if (!query.exec())
    {
        qDebug() << "标记会话已读失败" << query.lastError().text();
    }
}

/**
 * @brief DatabaseMsg::getRecentSessions
 * 取最近会话列表（好友 + 群组），按最后一条消息时间倒序。
 * 每条包含：isGroup/id/name/head/content/datetime/type/unread
 */
QVector<QJsonObject> DatabaseMsg::getRecentSessions()
{
    QVector<QJsonObject> sessions;
    if (!msgdb.isOpen())
    {
        return sessions;
    }

    // 好友会话：每个 user_id 只取最后一条消息，并顺带统计未读数和好友头像
    QSqlQuery friendQuery(msgdb);
    QString sql;
    sql = QString("SELECT m.user_id, m.name, m.content, m.datetime, m.type, ");
    sql.append("(SELECT COUNT(*) FROM MSGINFO x WHERE x.user_id = m.user_id AND x.unread = 1) AS unread, ");
    sql.append("(SELECT x.head FROM MSGINFO x WHERE x.user_id = m.user_id AND x.direction = 1 ORDER BY x.id DESC LIMIT 1) AS head ");
    sql.append("FROM MSGINFO m WHERE m.id IN (SELECT MAX(id) FROM MSGINFO GROUP BY user_id) ORDER BY m.id DESC;");
    if (friendQuery.exec(sql))
    {
        while (friendQuery.next())
        {
            const int friendId = friendQuery.value("user_id").toInt();
            QString name = getFriendName(friendId);
            if (name.isEmpty())
            {
                name = friendQuery.value("name").toString();
            }
            // 优先用好友表里记录的最新头像，其次才是最后一条消息里的头像
            QString head = getFriendHead(friendId);
            if (head.isEmpty())
            {
                head = friendQuery.value("head").toString();
            }
            if (head.isEmpty())
            {
                head = "default.png";
            }

            QJsonObject obj;
            obj.insert("isGroup", false);
            obj.insert("id", friendId);
            obj.insert("name", name);
            obj.insert("head", head);
            obj.insert("content", friendQuery.value("content").toString());
            obj.insert("datetime", friendQuery.value("datetime").toString());
            obj.insert("type", friendQuery.value("type").toInt());
            obj.insert("unread", friendQuery.value("unread").toInt());
            sessions.append(obj);
        }
    }
    else
    {
        qDebug() << "查询最近好友会话失败" << friendQuery.lastError().text();
    }

    // 群组会话
    QSqlQuery groupQuery(msgdb);
    sql = QString("SELECT m.group_id, m.content, m.datetime, m.type, ");
    sql.append("(SELECT COUNT(*) FROM GROUPMSGINFO x WHERE x.group_id = m.group_id AND x.unread = 1) AS unread ");
    sql.append("FROM GROUPMSGINFO m WHERE m.id IN (SELECT MAX(id) FROM GROUPMSGINFO GROUP BY group_id) ORDER BY m.id DESC;");
    if (groupQuery.exec(sql))
    {
        while (groupQuery.next())
        {
            const int groupId = groupQuery.value("group_id").toInt();
            QString name = getGroupName(groupId);
            if (name.isEmpty())
            {
                name = QString("群组%1").arg(groupId);
            }
            QString head = getGroupHead(groupId);
            if (head.isEmpty())
            {
                head = "default_group.png";
            }

            QJsonObject obj;
            obj.insert("isGroup", true);
            obj.insert("id", groupId);
            obj.insert("name", name);
            obj.insert("head", head);
            obj.insert("content", groupQuery.value("content").toString());
            obj.insert("datetime", groupQuery.value("datetime").toString());
            obj.insert("type", groupQuery.value("type").toInt());
            obj.insert("unread", groupQuery.value("unread").toInt());
            sessions.append(obj);
        }
    }
    else
    {
        qDebug() << "查询最近群组会话失败" << groupQuery.lastError().text();
    }

    // 按最后一条消息时间倒序（时间是 "MM-dd HH:mm" 文本，直接字符串比较即可）
    std::sort(sessions.begin(), sessions.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value("datetime").toString() > b.value("datetime").toString();
    });

    return sessions;
}

///////////////////////////////////////////////////////////////////////////
/// 群组管理
///

int DatabaseMsg::getGroupCreator(const int &groupId)
{
    QSqlQuery query(userdb);
    query.prepare("SELECT creator_id FROM GROUPS WHERE group_id = :groupId;");
    query.bindValue(":groupId", groupId);
    if (!query.exec() || !query.next()) {
        return -1;
    }
    return query.value("creator_id").toInt();
}

/**
 * @brief DatabaseMsg::setGroupIdentity
 * 群主转让后同步本地的角色，保证群列表右键菜单显示的"退群/解散"跟着变
 */
bool DatabaseMsg::setGroupIdentity(int groupId, int identity)
{
    if (groupId <= 0) return false;
    QSqlQuery query(userdb);
    query.prepare("UPDATE GROUP_MEMBER SET identity = :identity "
                  "WHERE group_id = :groupId AND user_id = :userId;");
    query.bindValue(":identity", identity);
    query.bindValue(":groupId", groupId);
    query.bindValue(":userId", MyApp::m_nId);
    if (!query.exec()) {
        qDebug() << "[群] 更新我的群角色失败：" << query.lastError().text();
        return false;
    }
    return true;
}

/**
 * @brief DatabaseMsg::setGroupCreator
 * 更新本地的群主记录（群列表菜单靠它判断"解散/退群"）
 */
bool DatabaseMsg::setGroupCreator(int groupId, int userId)
{
    if (groupId <= 0 || userId <= 0) return false;
    QSqlQuery query(userdb);
    query.prepare("UPDATE GROUPS SET creator_id = :userId WHERE group_id = :groupId;");
    query.bindValue(":userId", userId);
    query.bindValue(":groupId", groupId);
    if (!query.exec()) {
        qDebug() << "[群] 更新本地群主失败：" << query.lastError().text();
        return false;
    }
    return true;
}

int DatabaseMsg::getMyGroupIdentity(const int &groupId)
{
    QSqlQuery query(userdb);
    query.prepare("SELECT identity FROM GROUP_MEMBER WHERE group_id = :groupId AND user_id = :userId;");
    query.bindValue(":groupId", groupId);
    query.bindValue(":userId", MyApp::m_nId);
    if (!query.exec() || !query.next()) {
        return -1;      // 不在这个群里
    }
    return query.value("identity").toInt();
}

/**
 * @brief DatabaseMsg::getGroupMemberIds
 * 取群成员 id 列表，端到端加密时用来给每个成员包裹会话密钥
 */
QList<int> DatabaseMsg::getGroupMemberIds(const int &groupId)
{
    QList<int> ids;
    QSqlQuery query(userdb);
    query.prepare("SELECT user_id FROM GROUP_MEMBER WHERE group_id = :groupId;");
    query.bindValue(":groupId", groupId);
    if (!query.exec()) {
        qDebug() << "获取群成员列表失败" << query.lastError();
        return ids;
    }
    while (query.next()) {
        const int uid = query.value("user_id").toInt();
        if (uid > 0 && !ids.contains(uid)) ids.append(uid);
    }
    return ids;
}

bool DatabaseMsg::removeGroupMember(const int &groupId, const int &userId)
{    if (groupId < 0 || userId < 0) return false;
    QSqlQuery query(userdb);
    query.prepare("DELETE FROM GROUP_MEMBER WHERE group_id = :groupId AND user_id = :userId;");
    query.bindValue(":groupId", groupId);
    query.bindValue(":userId", userId);
    if (!query.exec()) {
        qDebug() << "本地删除群成员失败" << query.lastError();
        return false;
    }
    return true;
}

bool DatabaseMsg::removeGroup(const int &groupId)
{
    if (groupId < 0) return false;

    QSqlQuery query(userdb);
    query.prepare("DELETE FROM GROUP_MEMBER WHERE group_id = :groupId;");
    query.bindValue(":groupId", groupId);
    if (!query.exec()) {
        qDebug() << "本地删除群成员记录失败" << query.lastError();
        return false;
    }

    query.prepare("DELETE FROM GROUPS WHERE group_id = :groupId;");
    query.bindValue(":groupId", groupId);
    if (!query.exec()) {
        qDebug() << "本地删除群组失败" << query.lastError();
        return false;
    }
    return true;
}

/**
 * @brief DatabaseMsg::clearSessionMessages
 * 删除某个会话的全部聊天记录（只删消息，不动好友/群）
 */
bool DatabaseMsg::clearSessionMessages(const int &id, const bool &isGroup)
{
    if (id < 0) return false;

    QSqlQuery query(msgdb);
    if (isGroup) {
        query.prepare("DELETE FROM GROUPMSGINFO WHERE group_id = :id;");
    } else {
        query.prepare("DELETE FROM MSGINFO WHERE user_id = :id;");
    }
    query.bindValue(":id", id);
    if (!query.exec()) {
        qDebug() << "删除会话聊天记录失败" << query.lastError().text();
        return false;
    }
    return true;
}

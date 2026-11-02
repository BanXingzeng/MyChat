#ifndef DATABASEMSG_H
#define DATABASEMSG_H

#include <QMutex>
#include <QObject>
#include <QSqlDatabase>
#include <QVector>
#include <basewidget/iteminfo.h>
#include "comapi/unit.h"

class DatabaseMsg : public QObject
{
    Q_OBJECT
public:
    explicit DatabaseMsg(QObject *parent = nullptr);
    ~DatabaseMsg();

    // 单实例运行
    static DatabaseMsg *getInstance()
    {
        static QMutex mutex;
        if (nullptr == self) {
            QMutexLocker locker(&mutex);

            if (!self) {
                self = new DatabaseMsg();
            }
        }

        return self;
    }

    bool OpenUserDatabase(const QString &dataName);
    bool OpenMessageDatabase(const QString &dataName);
    QString getFriendName(const int &id);
    QString getFriendHead(const int &id);                       // 好友头像文件名
    void updateFriendHead(const int &friendId, const QString &head);   // 更新好友头像（含历史消息）

    // 好友操作
    void AddFriend(const int &userId, const QString &friendName ,int status);
    QJsonValue GetMyFriend();
    bool deleteFriend(const int &userId);

    // 消息操作
    void AddHistoryMsg(const int &userId, ItemInfo *itemInfo);
    void AddGroupHistoryMsg(const int &userId, const int &groupId, ItemInfo *itemInfo);
    QVector<QJsonObject> getHistoryMsg(const int &id, const int &count = 0);
    QVector<QJsonObject> getGroupHistoryMsg(const int &groupId, const int &count = 0);
    // 聊天记录窗口用：一次性取出某个会话的全部记录（按时间正序）
    QVector<QJsonObject> getAllHistoryMsg(const int &id);
    // 跨会话搜索聊天记录（私聊 + 群聊），返回统一结构供"全局搜索"使用
    QVector<QJsonObject> searchAllMessages(const QString &keyword, int limit = 300);
    QVector<QJsonObject> getAllGroupHistoryMsg(const int &groupId);
    bool isMyFriend(QString friendName);
    void removeFriend(const QString &friendName);

    // 未读消息统计
    int getUnreadCountByFriend(const int &friendId);            // 某个好友的未读数
    int getUnreadCountByGroup(const int &groupId);              // 某个群组的未读数
    int getUnreadTotal(const bool &isGroup);                    // 好友/群组未读总数
    void markSessionRead(const int &id, const bool &isGroup);   // 把某个会话标记为已读
    bool clearSessionMessages(const int &id, const bool &isGroup); // 删除某个会话的全部聊天记录
    bool deleteMsgByKey(const QString &msgKey);                 // 按消息唯一标识删除（撤回 / 删除）
    QVector<QJsonObject> getRecentSessions();                   // 取最近会话（好友 + 群组）

    //群组操作
    void AddGroup(const int &userId, const QString &groupHead, const QString &groupName, const int &groupId);
    QString getGroupName(const int &groupId);
    QString getGroupHead(const int &groupId);
    bool addGroupMember(const int &userId, const int &groupId, GroupIdentity identity = Member);
    void addGroupMember(const int &userId, const int &groupId, const QString &joinTime, GroupIdentity identity = Member);
    int  getGroupCreator(const int &groupId);                       // 群主 id，取不到返回 -1
    int  getMyGroupIdentity(const int &groupId);                    // 我在群里的身份，不在群里返回 -1
    bool setGroupIdentity(int groupId, int identity);               // 更新我在群里的角色（转让群主后同步）
    bool setGroupCreator(int groupId, int userId);                  // 更新本地记录的群主
    QList<int> getGroupMemberIds(const int &groupId);               // 群成员 id 列表（端到端加密用）
    bool removeGroupMember(const int &groupId, const int &userId);  // 退出群组
    bool removeGroup(const int &groupId);                           // 解散群组（本地删群 + 成员）


private:
    void ensureUnreadColumn(const QString &table);
    void ensureColorColumn(const QString &table);    // 文字消息颜色字段
    void fixPictureMsgType(const QString &table);    // 纠正被当成文字入库的图片消息
    void ensureMsgKeyColumn(const QString &table);   // msg_key / ts 字段
    void ensureDurationColumn(const QString &table); // duration 字段（语音时长）
    void ensureFriendHeadColumn();   // 老数据库为 FRIEND 表补 head 字段

    static DatabaseMsg *self;
    QSqlDatabase userdb;
    QSqlDatabase msgdb;

signals:

public slots:
};



#endif // DATABASEMSG_H

#ifndef DATABASEMAG_H
#define DATABASEMAG_H
#include <QMutex>
#include <QObject>
#include <QSqlDatabase>
#include "comapi/unit.h"

class DataBaseMag : public QObject
{
    Q_OBJECT
public:
    explicit DataBaseMag(QObject *parent = nullptr);
    ~DataBaseMag();

    static DataBaseMag* getInstance()
    {
        static QMutex mutex;
        if(self == nullptr){
            QMutexLocker locker(&mutex);
            if(!self){
                self = new DataBaseMag();
            }
        }
        return self;
    }

public:
    bool openDatabase(const QString &dataName);
    void closeDatabase();
    void createDatabase();
    void changeAllUserStatus(); //更新所有用户的状态

    // 注册用户
    E_STATUS userRegister(const QString &name, const QString &passwd);
    QJsonObject userLogin(const QString &name,const QString &passwd);
    bool isFriend(const int &userId,const QString &friendName);
    QString getUserHead(const int &userId);
    bool updateUserHead(const int &userId, const QString &head);   // 更新用户头像文件名

    bool haveUser(const QString &name);   //查询用户是否存在

    // ===== 服务器管理界面使用的用户管理接口 =====
    int  insertUser(const QString &name, const QString &passwd,
                    const QString &phone, const QString &workGroup,
                    const QString &email, const QString &head, QString *err = nullptr);   // 添加用户，返回新用户ID
    bool updateUserInfo(int id, const QString &name, const QString &passwd,
                        const QString &phone, const QString &workGroup,
                        const QString &email, const QString &head, QString *err = nullptr); // 修改用户资料
    bool deleteUser(int id, QString *err = nullptr);                      // 删除用户（级联删除好友/群成员）
    bool checkPassword(const QString &name, const QString &passwd);       // 校验密码（不改变在线状态）
    bool updatePassword(const QString &name, const QString &newPasswd);   // 修改密码
    QVariantMap getUserInfoByName(const QString &key);                    // 按用户名/ID 查询用户详情
    QVariantMap getUserInfoById(int id);                                  // 按 ID 查询用户详情
    int getUserCount();                                                   // 用户总数
    int getOnlineUserCount();                                             // 在线用户数
    int getFriendCount(int userId);                                       // 好友数量
    int getGroupCount(int userId);                                        // 已加入群组数量

    void UpdateUserStatus(int id, E_STATUS status, bool touchTime = true);
    QList<QVariantMap> getAllUser();
    int getIdFromUsername(const QString &userName); //通过好友用户名获取id
    QString getUsernameFromId(const QString &userId);   //通过好友Id获取用户名

    QString jsonValueToString(const QJsonValue &jsonVal);
    void insertMessageQueue(const int &senderId, const int &acceptId, quint8 type, const QJsonValue &dataVal);
    QList<QVariantMap> getUserMessageQueue(const int &userId);
    void addFriend(const int &userId, const int &friendId);
    void deleteFriend(const int &userId, const int &friendId);
    bool isOnline(const int &userId);
    QJsonValue getMyFriends(const int &userId);
    QJsonValue getMyGroup(const int &userId);
    int addGroup(const int &userId, const QString groupName);
    bool addGroupMember(const int &userId, const int &groupId, GroupIdentity identity = Member);
    bool removeGroupMember(const int &groupId, const int &userId);   // 退出群组
    bool removeGroup(const int &groupId);                            // 解散群组（删群 + 所有成员）
    int getGroupOwner(const int &groupId);
    QString getGroupName(const int &groupId);
    QString getGroupHead(const int &groupId);

    QHash<int, QSet<int>> initGroupMembersCache();
    QJsonValue getGroupMember(const int &groupId);
    QJsonValue getGroupMemberDetail(const int &groupId);            // 成员详情（昵称/头像/在线/角色）
    bool isGroupOwner(const int &groupId, const int &userId);       // 是否群主
    bool setGroupMemberIdentity(const int &groupId, const int &userId, const int &identity);
    bool setGroupCreator(const int &groupId, const int &userId);    // 转让群主

    // ===== 端到端加密：用户公钥目录 =====
    // 服务器只保存公钥（公开信息），不保存任何私钥 → 服务器无法解密任何正文。
    bool updateUserKeys(int id, const QString &pubKey);
    QString getUserPubKey(int id);                        // 用户公钥 PEM（空=未上传）

    // ===== 端到端加密：消息审计 =====
    void insertMsgLog(int fromId, int toId, bool isGroup, quint8 type, const QString &payload);
    QList<QVariantMap> getMsgLog(int limit = 500);
    int getMsgLogCount();

signals:

public slots:

private:
    static DataBaseMag* self;
    QSqlDatabase userdb;

    void queryAll();
    void ensureUserInfoColumns();   // 老库自动补齐 phone / work_group / email 列
    int  migratePlainPasswords();   // 历史明文口令转加盐哈希，返回迁移条数
};
#endif // DATABASEMAG_H

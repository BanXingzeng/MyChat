#ifndef CLIENTSOCKET_H
#define CLIENTSOCKET_H

#include <QObject>
#include <QTcpSocket>
#include <QFile>
#include <QApplication>
#include <QDateTime>

class SecureChannel;

// 服务端socket管理类
class ClientSocket : public QObject
{
    Q_OBJECT
public:
    explicit ClientSocket(QObject *parent = 0, QTcpSocket *tcpSocket = NULL);
    ~ClientSocket();

    int GetUserId();
    void Close();
    void kick();                                        // 主动断开（顶号/心跳超时）
    QDateTime lastActiveTime() const;                   // 最近一次收到数据的时刻
    void sendMsgType(const quint8 &nType, const QJsonValue &dataVal);
    /// 该连接是否已完成 v2 握手（加密通道就绪）。未就绪时只接受握手帧。
    bool isSecure() const;
signals:
    void signalConnected();
    void signalDisConnected(ClientSocket *client);
    void signalDownloadFile(const QJsonValue &json);
    void signalLoginSuccess(ClientSocket *client, const QString &userId);
    //id发给targetId
    void signalPrivateMsgToClient(const int &id,const int &targetId, const quint8 &type, const QJsonValue &json);
    void signalGroupMsgToClient(const int &id, const int &groupId, const quint8 &type, const QJsonValue &json);
    void signalAddGroupMembers(const int &groupId, const int &userId);
    void signalRemoveGroupMembers(const int &groupId, const int &userId);
    // 某个用户换了头像，需要广播给其它在线用户
    void signalHeadChanged(const int &id, const QJsonValue &json);

public slots:

private:
    QTcpSocket *m_tcpSocket;
    int m_nId;  //储存用户的id（唯一字段）识别每个套接字对应的用户
    QDateTime m_lastActive;   // 最近一次收到数据的时刻（心跳超时判定）
    QByteArray m_recvBuffer;

    // ===== 协议 v2：链路加密 =====
    SecureChannel *m_channel = nullptr;
    /// 加长度头并写出（握手帧与密文帧共用）
    void writeFrame(const QByteArray &payload);

    // ===== 协议 v2：挑战-应答登录的中间状态 =====
    QString    m_authName;      // 挑战发给谁
    int        m_authUid = -1;
    QByteArray m_authSalt;
    QByteArray m_authVerifier;  // 库里存的 digest，只用来重算 HMAC
    QByteArray m_authNonce;     // 本次挑战的随机数，一次一用

public slots:
    // 消息回发
    void SltSendMessage(const quint8 &type, const QJsonValue &json);

private slots:
    void SltConnected();
    void SltDisconnected();
    void SltReadyRead();

private:
    // 消息解析和抓转发处理
    void ParseLogin(const QJsonValue &dataVal);     //同步回复
    /// 挑战-应答第一阶段：下发 salt / 迭代次数 / 一次性 nonce
    void ParseLoginInit(const QJsonObject &dataObj);
    /// 挑战-应答第二阶段：校验 HMAC proof（以及可选的用户身份私钥签名）
    void ParseLoginProof(const QJsonObject &dataObj);
    /// 认证通过后的统一收尾：在线判定、顶号、下发好友与群
    void completeLogin(QJsonObject jsonObj);
    void ParseUserOnline(const QJsonValue &dataVal);
    void ParseLogout(const QJsonValue &dataVal);
    void ParseUpdateUserHead(const QJsonValue &dataVal);   // 上传头像
    void ParseGetUserHead(const QJsonValue &dataVal);      // 拉取头像

    void ParseReister(const QJsonValue &dataVal);
    void ParseAddFriend(const QJsonValue &dataVal);
    void ParseAddFriendReply(const QJsonValue &dataVal);
    void ParseAddGroupRequist(const QJsonValue &dataVal);
    void ParseAddGroupReply(const QJsonValue &dataVal);
    void ParseCreateGroup(const QJsonValue &dataVal);

    void ParseGetMyFriend(const QJsonValue &dataVal);
    void ParseGetMyGroups(const QJsonValue &dataVal);

    void ParseDeleteMyFriend(const QJsonValue &dataVal);

    void ParseRefreshFriend(const QJsonValue &dataVal);
    void ParseRefreshGroups(const QJsonValue &dataVal);

    void ParseFriendMessages(const QByteArray &reply);
    void ParseGroupMessages(const QByteArray &reply);
    void ParseFaceMessages(const QByteArray &reply);

    void ParseSendMsg(const QJsonValue &dataVal);   //发送信息
    void ParseSendFace(const QJsonValue &dataVal);  //发送表情
    void ParseSendFile(const QJsonValue &dataVal);
    void ParseSendGroupMsg(const QJsonValue &dataVal);
    void ParseRecallMsg(const QJsonValue &dataVal);          // 撤回消息

    // 消息已受理的回执：把消息 key 回给发送者，客户端据此把气泡标为"已送达"
    void sendAck(const QJsonObject &jsonObj);

    void ParseQuitGroup(const QJsonValue &dataVal);       // 退出群组
    void ParseDissolveGroup(const QJsonValue &dataVal);   // 解散群组（仅群主）

    // ===== 群成员管理 =====
    void ParseGetGroupMembers(const QJsonValue &dataVal);       // 拉取群成员列表
    void ParseKickGroupMember(const QJsonValue &dataVal);       // 群主移出成员
    void ParseTransferGroupOwner(const QJsonValue &dataVal);    // 群主转让
    void broadcastGroupMembersChanged(int groupId, int ownerId); // 通知群内成员刷新
    void ParseChangePasswd(const QJsonValue &dataVal);    // 修改密码

    // ===== 端到端加密：密钥交换 =====
    void ParseUploadKeys(const QJsonValue &dataVal);      // 上传公钥 + 托管私钥
    void ParseGetPubKey(const QJsonValue &dataVal);       // 获取指定用户公钥
};


#endif // CLIENTSOCKET_H

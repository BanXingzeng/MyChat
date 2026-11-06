#ifndef UNIT
#define UNIT

#include <stdint.h>
#include <QString>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>

#define MY_VERSION          0x01000001
#define MY_VERSION_STR      "1.0.0.1"

// 版本检测
#define VERSION_CHECK(a,b,c,d)  ((a << 24) | (b << 16) | (c << 8) | d)

#define MSG_PORT_ONLINE     6353
#define MSG_PORT_MESSAGE    5363
#define TCP_FILE_PORT       7777

/* IP Messenger Communication Protocol version 1.0 define */
/* macro */
#define GET_MODE(command)    (command & 0x000000ffUL)
#define GET_OPT(command)    (command & 0xffffff00UL)

/* header */
#define IPMSG_VERSION            0x0001
#define IPMSG_DEFAULT_PORT        0x0979

/* command */
#define IPMSG_NOOPERATION       0x00000000      // 不进行任何操作

#define IPMSG_BR_ENTRY          0x00000001      // 用户上线
#define IPMSG_BR_EXIT           0x00000002      // 用户退出
#define IPMSG_ANSENTRY          0x00000003      // 通报在线
#define IPMSG_BR_ABSENCE        0x00000004
#define IPMSG_REGISTER          0x00000005      // 注册

#define IPMSG_BR_ISGETLIST      0x00000010
#define IPMSG_OKGETLIST         0x00000011
#define IPMSG_GETLIST           0x00000012
#define IPMSG_ANSLIST           0x00000013
#define IPMSG_FILE_MTIME        0x00000014
#define IPMSG_FILE_ANSWER       0x00000015
#define IPMSG_FILE_CREATETIME   0x00000016
#define IPMSG_BR_ISGETLIST2     0x00000018

#define IPMSG_SENDMSG           0x00000020      // 发送消息
#define IPMSG_RECVMSG           0x00000021      // 通报收到消息

#define IPMSG_READMSG           0x00000030      // 用户读取消息
#define IPMSG_DELMSG            0x00000031      // 撤销消息

/* option for all command */
#define IPMSG_ABSENCEOPT        0x00000100
#define IPMSG_SERVEROPT         0x00000200
#define IPMSG_DIALUPOPT         0x00010000
#define IPMSG_FILEATTACHOPT     0x00200000

/* file types for fileattach command */
#define IPMSG_FILE_REGULAR        0x00000001UL
#define IPMSG_FILE_DIR            0x00000002UL
#define IPMSG_LISTGET_TIMER       0x0104
#define IPMSG_LISTGETRETRY_TIMER  0x0105

/* 聊天气泡相关设置 */
#define ITEM_START_ANGLE 270
#define ITEM_D_ANGLE 2
#define ITEM_D_ZOOMING_FACTOR 0.05
#define UPDATE_TIMER_INTERVAL 10 // ms
#define ITEM_COUNTER_TIMER_INTERVAL 200
#define SIZE_HINT QSize(300,300)

#define ITEM_HEIGHT 40
#define BG_START_COLOR QColor(179,179,183)
#define BG_END_COLOR QColor(187,193,207)

#define ITEM_START_COLOR ITEM_END_COLOR.lighter()
#define ITEM_END_COLOR QColor(62,135,166)

#define ITEM_START_COLOR_A ITEM_END_COLOR_A.lighter()
#define ITEM_END_COLOR_A QColor(221,221,221)

#define HOVER_OPACITY 0.2
#define HOVER_START_COLOR HOVER_END_COLOR.lighter()
#define HOVER_END_COLOR QColor(98,139,21)

#define HOVER_TEXT_COLOR Qt::red
#define SELECT_TEXT_COLOR Qt::white
#define NORMAL_TEXT_COLOR Qt::black

#define RECT_RADIUS 4

#define ITEM_SPACE          20
#define ITEM_TITLE_HEIGHT   20

#define  DATE_TIME QDateTime::currentDateTime().toString("MM-dd HH:mm")
#define BATCH_SIZE 50


// 气泡方向
typedef enum {
    None,
    Left,
    Right
} Orientation;

typedef enum {
    Member,
    Owner,
} GroupIdentity;

typedef enum {
    Text,           // 普通文字消息
    Audio,          // 语音消息
    Picture,        // 图片消息
    Files,          // 文件传输
    Face,           //动画表情
} MessageType;

// 自己发出的消息的送达状态（气泡左下角的小图标）
typedef enum {
    SendSending,    // 已发出，等服务端回执
    SendDone,       // 服务端已受理
    SendFailed,     // 超时 / 断线，右键可重新发送
} SendState;


typedef enum {
    Unknow,
    Register            = 0x10,     // 用户注册
    Login,                          // 用户登录
    Logout,                         // 用户注销
    LoginRepeat,                    // 重复登录

    UserOnLine          = 0x15,     // 用户上线通知
    UserOffLine,                    // 用户下线通知
    UpdateHeadPic,                  // 用户更新头像

    AddFriend           = 0x20,     // 添加好友
    AddGroup,                       // 添加群组

    AddFriendRequist,               // 添加好友确认通知
    AddGroupRequist,                // 添加群组确认通知

    AddFriendReply,                 //添加好友回复
    AddGroupReply,
    AddGroupAccept,

    CreateGroup         = 0x27,     // 创建群组

    GetMyFriends        = 0x30,     // 上线获取我的好友的状态
    GetMyGroups,                    // 获取我的群组信息

    RefreshFriends      = 0x35,     // 刷新好友状态
    RefreshGroups,                  // 刷新群组成员状态

    SendMsg             = 0x40,     // 发送消息
    SendGroupMsg,                   // 发送群组消息
    SendFile,                       // 发送文件
    SendPicture,                    // 发送图片
    SendFace,                       // 发送表情

    ChangePasswd        = 0x50,     // 修改密码

    DeleteFriend        = 0x55,     // 删除好友
    DeleteGroup,                    // 退出群组

    SendFileOk          = 0x60,     // 文件发送完成状态

    GetFile             = 0x65,     // 获取文件（到服务器下载文件）
    GetPicture,                     // 图片下载
    GetUserHead         = 0x67,     // 获取用户头像（本地缺文件时向服务器拉取）

    // ===== 端到端加密：密钥交换 =====
    GetPubKey           = 0x68,     // 客户端 → 服务端：请求指定用户的公钥
    UploadKeys          = 0x69,     // 客户端 → 服务端：登记本人公钥（只传公钥，私钥永不外发）
    HeartBeat           = 0x6A,     // 客户端 → 服务端：心跳（用于判定真实在线状态）
    RecallMsg           = 0x6B,     // 撤回消息（携带 msg_key，服务端只做转发）

    // ===== 群成员管理 =====
    GetGroupMembers     = 0x6C,     // 客户端 → 服务端：请求群成员列表
    KickGroupMember     = 0x6D,     // 客户端 → 服务端：群主把某人移出群聊
    TransferGroupOwner  = 0x6E,     // 客户端 → 服务端：群主转让群主身份

} E_MSG_TYPE;

typedef enum {
    ConnectedHost = 0x01,
    DisConnectedHost,

    LoginSuccess,       // 登录成功
    LoginPasswdError,   // 密码错误

    OnLine,
    OffLine,

    RegisterOk,
    RegisterFailed,

    AddFriendFailed_NoneUser,   //用户不存在
    AddFriendFailed_IsHad,  //已经有好友了状态为1
    AddFriendFailed_Readd,  //重复添加状态为0(申请还未同意/拒绝)

    // 群组管理
    QuitGroup,              // 客户端 → 服务端：退出群组
    QuitGroupOk,            // 服务端 → 客户端：退群成功
    DissolveGroup,          // 客户端 → 服务端：解散群组（仅群主）
    DissolveGroupOk,        // 服务端 → 客户端：解散成功（群主）
    GroupDissolved,         // 服务端 → 客户端：群已解散（通知其它成员）
    GroupOperateFailed,     // 服务端 → 客户端：群操作失败

    // 头像
    UpdateHeadOk        = 0x70,     // 服务端 → 客户端：自己的头像更新成功
    HeadChanged         = 0x71,     // 服务端 → 客户端：某个用户换了头像
    HeadData            = 0x72,     // 服务端 → 客户端：头像图片内容（base64）
    HeadOperateFailed   = 0x73,     // 服务端 → 客户端：头像操作失败
    ChangePasswdOk      = 0x74,     // 服务端 → 客户端：密码修改成功
    ChangePasswdFailed  = 0x75,     // 服务端 → 客户端：密码修改失败

    // ===== 端到端加密：公钥交换 =====
    // 注意：0x76 原为 AdminPubKey（管理员公钥下发），端到端改造后服务器不持有任何私钥，
    // 该报文已彻底移除，编号有意留空不再复用，避免与历史日志混淆。
    UploadKeysOk        = 0x77,     // 服务端 → 客户端：公钥上传成功
    PubKeyData          = 0x78,     // 服务端 → 客户端：用户公钥列表
    KeyOperateFailed    = 0x79,     // 服务端 → 客户端：密钥操作失败
    HeartBeatAck        = 0x7A,     // 服务端 → 客户端：心跳应答

    // ===== 群成员管理 =====
    GroupMembers        = 0x7B,     // 服务端 → 客户端：群成员列表
    GroupMemberChanged  = 0x7C,     // 服务端 → 客户端：群成员有变动（踢人/退群/转让）
    GroupKicked         = 0x7D,     // 服务端 → 客户端：你已被移出该群

    // ===== 发送回执 =====
    // 服务端收到并受理一条消息后，把消息里的 key 原样回给发送者。
    // 客户端据此把气泡置为"已送达"，超时没收到回执就标红提示可重发。
    SendAck             = 0x7E,

    // 服务端拒绝转发（如对方不是好友）：带 key 与原因，客户端把气泡标红
    SendRejected        = 0x7F,
} E_STATUS;

#endif // UNIT


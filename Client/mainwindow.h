#ifndef MAINWINDOW_H
#define MAINWINDOW_H
#include "basewidget/customwidget.h"
#include "netdb/mysocket.h"

#include <QSystemTrayIcon>
#include <QHash>
#include <QSet>
#include <QJsonValue>
#include <QJsonObject>
class QButtonGroup;
class QProcess;
class QLabel;
class QQListWidget;
class QQPopMenuWidget;
class QMenu;
class QListWidget;
class GroupMemberDialog;
class QQCell;

// 待处理的申请（好友申请 / 加群申请）
struct PendingRequest
{
    quint8  type = 0;      // AddFriendRequist / AddGroupRequist
    int     id = -1;       // 申请人 id
    QString name;          // 申请人名字
    int     groupId = -1;  // 加群申请时的群号
    QString groupName;     // 加群申请时的群名
};

// 等待对方公钥期间暂存的待发消息（端到端加密）
struct PendingMsg
{
    quint8     type = 0;   // SendMsg / SendGroupMsg
    QJsonValue data;       // 明文消息体
    int        retry = 0;  // 已重试次数
};

// 本机密钥还没就绪时收到的密文消息先压在这里，等密钥就绪后按原顺序重放。
// 不进本地库、不写"无法解密"，否则密文一旦被错误文本覆盖就再也恢复不了。
struct PendingIncoming
{
    quint8     type = 0;   // SendMsg / SendGroupMsg
    QJsonValue data;       // 服务器下发的原始消息体（msg 里仍是密文包）
    int        waitMs = 0; // 已经等待的毫秒数
};
class QJsonObject;
class QCloseEvent;
class QQCell;
class ChatWindow;
class QTimer;namespace Ui {
class MainWindow;
}

class MainWindow : public CustomMoveWidget
{
    Q_OBJECT

public:
    explicit MainWindow(MySocket *socket, QWidget *parent = nullptr);
    ~MainWindow();

//    void addFriend(const QJsonValue &dataVal);
    void addFriendRequist(const QJsonValue &dataVal);
    void addFriendReply(const QJsonValue &dataVal);
    void deleteFriend(const QJsonValue &dataVal);
    void showServerFriendInfo(const QJsonValue &dataVal);   //显示服务器发送过来的好友信息
    void showServerGroupsInfo(const QJsonValue &dataVal);   //显示服务器发送来得到群组信息
    void receiveMessage(const QJsonValue &dataVal);     //解析消息数据
    void createGroup(const QJsonValue &dataVal);
    void addGroupRequist(const QJsonValue &dataVal);
    void addGroupReply(const QJsonValue &dataVal);
    void addGroupAccept(const QJsonValue &dataVal);
    void sendGroupMsg(const QJsonValue &dataVal);
private slots:
    void sltButtonClicked(int index);
    void SltTrayIcoClicked(QSystemTrayIcon::ActivationReason reason);   //托盘图标
    void SltTrayIconMenuClicked(QAction* action);   //托盘图标菜单
    void SltQuitApp();  //退出应用

    void SltSysmenuCliecked(QAction* action);  //实现系统菜单功能

    void setHead(const QString &headFile);  //设置头像
    void onAddFriendMenuDidSelected(QAction* action);
    void SltFriendsClicked(QQCell *cell);
    void onChildPopMenuDidSelected(QAction* action);
    void onGroupPopMenuDidSelected(QAction* action);

    void sltStatus(const quint8 &status,const QJsonValue &dataVal);

    void SltFriendChatWindowClose();
    void SltGroupChatWindowClose();
    void SltGroupsClicked(QQCell *cell);
    void onGroupMenuDidSelected(QAction *action);

    void SltConversationClicked(QQCell *cell);   // 会话列表双击
    void SltGroupMenuWillShow(QQListWidget *listWidget, QQPopMenuWidget *widget, QMenu *menu);  // 群组右键菜单弹出前
    void on_tbtnAddUser_clicked();      // 点"添加好友"看申请列表
    void onConvMenuSelected(QAction *action);   // 会话列表右键菜单
    void on_tbtnWeather_clicked();     // 打开天气小程序
    void on_tbtnCalendar_clicked();    // 打开日历
    void on_btnWinMenu_clicked();      // 标题栏"设置"按钮
    void on_btnWinMin_clicked();       // 标题栏最小化
    void on_btnWinClose_clicked();     // 标题栏关闭（收到托盘）
    void SltAppCellClicked(QQCell *cell);   // 应用页双击
    void uploadHeadFile(const QString &filePath);   // 处理并上传指定图片文件（右键菜单/测试共用）

    // ===== 端到端加密 =====
    void sltChatSendMessage(const quint8 &type, const QJsonValue &dataVal);   // 聊天窗口发消息（先加密）
    void sltFlushPendingMessages();                       // 公钥到齐后补发
    void sltRetryPendingMessages();                       // 定时重试（对方可能稍后才上传公钥）

    // ===== 断线重连 =====
    void sltServerReconnected();                          // 链路恢复：自动重新登录
    void sltRelogin();                                    // 发一次登录请求
    void sltReloginTimeout();                             // 登录没回执：再试一次

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;   // 按钮尺寸变化时重摆未读角标

private:
    // 添加系统菜单处理
    void InitSysMenu();
    void InitQQListMenu();
    void InitSysTrayIcon();

    void startWeather();    // 启动天气程序（已启动则不重复启动）
    void stopWeather();     // 关闭随主程序启动的天气程序

    void updateTabBadge(QWidget *btn, int unread);   // 标签右上角未读角标
    void refreshUnreadBadges();                      // 统计未读并刷新"会话"页的标记
    void refreshConversationList();                  // 重建会话列表（最近会话）
    QString sessionPreview(const QJsonObject &obj);  // 会话列表里的最后一条消息预览文字
    ChatWindow *openChatWindow(QQCell *cell, bool isGroup, bool ownCell);   // 打开聊天窗口
    void removeGroupCell(int groupId);   // 从群组列表移除某个群（退出/解散后）

    void addPendingRequest(const PendingRequest &req);   // 收到申请：入队 + 红点
    void rebuildRequestPage();                           // 重建申请列表页
    void showRequestPage();                              // 切到申请列表页
    void handleRequest(const PendingRequest &req, bool accept);   // 同意/拒绝
    void notifyTray(const QString &title, const QString &content); // 托盘气泡提示（不弹模态框）

    // ===== 窗口 / 托盘 =====
    void hideToTray();                     // 隐藏到托盘（首次给出气泡提示）
    bool isWindowShowing() const;          // 是否处于"已显示且未最小化"
    void updateTrayTip();                  // 托盘悬停提示：用户名 + 连接状态
    void setConnState(bool online, const QString &detail = QString());   // 断线/恢复时的界面提示

    // ===== 系统菜单 / 应用页 =====
    void initAppPage();                    // 应用页列表（天气/日历/消息管理…）
    void openGroupMembers(QQCell *cell);   // 群成员管理窗口
    void openServerConfig();               // 系统设置：服务器地址端口
    void openMessageManager();             // 消息管理器
    void openFileHelper();                 // 文件助手（打开接收目录）
    void openChangePasswd();               // 修改密码
    void openAbout();                      // 关于
    void reconnectServer();                // 立即重连服务器
    void checkUpgrade();                   // 检查更新
    void showCalendar();                   // 日历窗口

    // ===== 头像 =====
    void showHeadMenu(const QPoint &globalPos);            // 右键自己的头像弹出的菜单
    void changeHead();                                     // 选图片 -> 上传服务器
    QString saveHeadImage(const QString &head, const QString &base64Data);    // 把服务器下发的头像存到本地
    void ensureHeadAvailable(int userId, const QString &head);  // 本地缺头像文件时向服务器拉取
    void requestHead(int userId);                          // 发 GetUserHead 请求
    void refreshHeadUi(int userId);                        // 头像变化后刷新列表/会话/聊天窗口

    // ===== 端到端加密 =====
    void initLocalCrypto();                                // 登录后立即在本地加载/生成本机密钥对（不等服务器）
    void uploadKeys();                                     // 把本机公钥登记到服务器（只传公钥）
    // 收消息入口：密文包在此解密，本机密钥未就绪时暂存等待，与聊天窗口是否打开无关
    void deliverIncoming(quint8 type, const QJsonValue &dataVal);
    QJsonValue decryptIfNeeded(const QJsonValue &dataVal, int *result = nullptr); // result:0正常 1待密钥 2解密失败
    void sltFlushPendingIncoming();                        // 密钥就绪后重放暂存的密文消息
    void handleRecallMsg(const QJsonValue &dataVal);       // 对端撤回某条消息
    void handleSendAck(const QJsonValue &dataVal);          // 服务端回执：我发的消息已被受理
    void failAllPendingSends(const QString &reason);       // 断线：把待回执消息全部标红
    void requestPubKeys(const QList<int> &ids);            // 向服务器索取对端公钥
    void prefetchChatKeys(ChatWindow *window, bool isGroup, int peerId);  // 打开会话时预热密钥
    bool encryptOutgoing(QJsonValue &dataVal, quint8 type, int *reason = nullptr);  // 把明文替换成密文包（reason:0成功 1等公钥 2失败）

    Ui::MainWindow *ui;

    QButtonGroup *m_buttonGroup;
    MySocket *m_tcpSocket;

    QSystemTrayIcon *systemTrayIcon;
    QProcess *m_weatherProcess;     // 天气小程序进程，随主程序退出而结束
    QHash<QWidget *, QLabel *> m_tabBadges;   // 标签按钮 -> 未读角标
    QList<PendingRequest> m_pendingRequests;  // 待处理的好友/加群申请
    QWidget *m_requestPage = nullptr;         // 申请列表页
    QListWidget *m_requestList = nullptr;     // 申请列表控件
    bool m_bQuit;   // 主动退出操作时不进行断线匹配
    QList<ChatWindow *> m_chatFriendWindows;
    QList<ChatWindow *> m_chatGroupWindows;
    QSet<int> m_headRequests;        // 正在向服务器拉头像的用户，避免重复请求
    QString m_strPendingPasswd;      // 刚提交的新密码（改成功后同步到本地配置）
    bool m_bTrayTipShown = false;    // 是否已提示过"最小化到托盘"
    qint64 m_msLastTrayClick = 0;    // 上次单击托盘的时间，去掉双击的重复触发

    // ===== 端到端加密 =====
    bool m_bCryptoReady = false;                 // 本机密钥是否已就绪并上传
    QList<PendingMsg> m_pendingSendMsgs;         // 等待公钥期间暂存的消息
    QSet<int> m_pendingKeyRequests;              // 已发出但还没回的公钥请求
    QTimer *m_sendRetryTimer = nullptr;          // 待发消息重试定时器
    QList<PendingIncoming> m_pendingIncoming;    // 密钥未就绪时暂存的收到消息
    QTimer *m_heartbeatTimer = nullptr;          // 心跳定时器（让服务器能判断本端是否存活）
    QHash<int, GroupMemberDialog *> m_groupMemberDialogs;   // 群号 → 已打开的成员窗口
    QTimer *m_reloginTimer = nullptr;            // 重连后等登录回执的超时定时器
    int     m_nReloginTries = 0;                 // 自动重新登录已尝试次数

};

#endif // MAINWINDOW_H

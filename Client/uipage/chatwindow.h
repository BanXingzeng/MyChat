#ifndef CHATWINDOW_H
#define CHATWINDOW_H

#include "basewidget/customwidget.h"
#include <QStandardItemModel>
#include <QFont>
#include <QColor>
#include <QJsonObject>
#include <QHash>
#include <QSet>

#include <QTcpSocket>
#include <QTime>
#include <qqcell.h>

#include <basewidget/chatbubble.h>

#include <uipage/face/emojiitem.h>

#include <netdb/filesocket.h>

namespace Ui
{
    class ChatWindow;
}

class QToolButton;
class QTimer;
class AudioRecorder;
class AudioRecordDialog;

class ChatWindow : public CustomMoveWidget
{
    Q_OBJECT

public:
    explicit ChatWindow(CustomMoveWidget *parent = nullptr);
    ~ChatWindow();

    void setCell(QQCell *cell, const quint8 &type = 0);
    void setCellOwned(QQCell *cell);      // 传入堆上新建的 QQCell，窗口析构时一并释放
    //    QString getAddr() const;
    int getUserId();
    QString getUserName();
    QString getUserHead();
    void AddMessage(const QJsonValue &jsonVal);
    QVector<ItemInfo *> getHistoryMsg();
    void SetIsGroup();
    bool getIsGroup();
public slots:
    void sltWheelUp();
    void sendFaceMsg(int index);
    void sltDownloadFile(const QString &fileName);

    // 撤回 / 删除后按 msg_key 移除消息（本窗口 + 数据库）
    // fromPeer=true 表示是对端撤回，只删本地、不再往外发
    void removeMsgByKey(const QString &msgKey, bool fromPeer = false);

    // 服务端回执：把对应气泡从"发送中"改成"已送达"
    bool hasMsgKey(const QString &msgKey) const;   // 本窗口里有没有这条消息
    void onSendAck(const QString &msgKey);
    /// 服务端明确拒绝转发（如非好友），把这条气泡标红
    void failSendByKey(const QString &msgKey);
    // 链路断开时把还在等回执的消息全部标成发送失败
    void failPendingSends(const QString &reason);

signals:
    void signalSendMessage(const quint8 &type, const QJsonValue &dataVal);
    void signalClose();
    void signalSessionChanged();   // 会话内容/未读状态发生变化，主窗口据此刷新会话列表
    void signalStartSend();
    void signalStopSend();
    void signalConnectFileServer(const QString &host, quint16 port);

protected:
    void changeEvent(QEvent *event) override;
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    //    QString GetHeadPixmap(const QString &name) const;

    void loadChatAppearance();     // 读取上次使用的字体 / 颜色
    void saveChatAppearance();     // 保存字体 / 颜色
    void applyChatFont();          // 把 m_chatFont 应用到输入框和消息区

    // 图片消息：统一走「压缩 -> base64 -> 加密消息」的路径
    void sendImageFile(const QString &filePath);
    void sendImagePixmap(const QPixmap &pixmap);
    void appendImageBubble(const QString &localPath, const QString &msgKey);

    // 语音消息：WAV -> base64 -> 消息通道（同样端到端加密）
    void sendAudioMessage(const QByteArray &wav, int durationMs);
    void appendAudioBubble(const QString &localPath, const QString &msgKey, int durationMs);
    void closeAudioDialog();        // 收起录音浮窗

    // 每条消息的唯一标识（收发双方一致，撤回时用来定位）
    static QString newMsgKey();

    // 登记"已发出、等回执"的消息：超时没收到回执就标红，右键可重发
    void beginSendTracking(const QString &msgKey, const QJsonObject &payload);
    // 按消息类型决定它该走哪条协议（文字/图片/语音走 SendMsg，文件走 SendFile，表情走 SendFace）
    quint8 protoOfPayload(const QJsonObject &payload);

    QToolButton *m_btnAudio = nullptr;      // 工具栏语音按钮（代码插入，.ui 里没有）
    AudioRecorder *m_recorder = nullptr;
    AudioRecordDialog *m_audioDialog = nullptr;
    QTimer *m_audioTimer = nullptr;         // 录音计时
    QString m_playingKey;                   // 当前正在播放的语音


private slots:
    // 文件
    //    void SltFileRecvFinished(const quint8 &type, const QString &filePath);
    //    void SltUpdateProgress(quint64 bytes, quint64 total);
    //    void SltDownloadFiles(const QString &fileName);

    void on_btnWinClose_clicked();

    void on_btnSendMsg_clicked();

    void on_btnSendFile_clicked();
    void on_btnClose_clicked();

    void on_toolButton_3_clicked();   // 表情
    void on_toolButton_2_clicked();   // 修改聊天字体
    void on_toolButton_4_clicked();   // 设置字体颜色
    void on_toolButton_7_clicked();   // 发送图片
    void on_toolButton_6_clicked();   // 截屏
    void on_toolButton_clicked();     // 聊天记录

    void sltMsgTextChanged();         // 输入框字符计数

    void sltCopyText(const QString &text);        // 右键"复制"
    void sltRecallMsg(const QString &msgKey);     // 右键"撤回"（2 分钟内）
    void sltDeleteMsg(const QString &msgKey);     // 右键"删除"
    void sltResendMsg(const QString &msgKey);     // 右键"重新发送"（发送失败的消息）

    // ---- 语音 ----
    void sltAudioBtnClicked();      // 语音按钮：开始录音 / 结束并发送
    void sltAudioTick();            // 更新录音浮窗计时 + 60 秒上限
    void sltAudioSend();
    void sltAudioCancel();
    void sltPlayAudio(const QString &msgKey, const QString &path);
    void sltAudioFinished(const QString &path);   // 播放结束，清掉播放中的高亮

    // ---- 发送状态 ----
    void sltAckCheckTimeout();      // 每秒扫一次：超时的消息标红

private:
    Ui::ChatWindow *ui;

    QQCell *m_cell = nullptr;
    QStandardItemModel *m_model = nullptr;

    //    FileSocket *m_tcpFileSocket; //文件传输
    //    QString m_strFileName;
    //    QTime m_updateTime;

    //    quint8 m_nFileType;
    quint8 m_nChatType; // 聊天类型，群组聊天或私人聊天
    bool m_isEnterSend = true;
    bool m_isGroup;
    bool m_ownCell = false;     // m_cell 是否为本窗口创建，析构时需要释放
    int m_nHistoryPage = 0;     // 已经加载过多少页历史（每页 10 条），用于分页偏移

    QFont   m_chatFont;         // 聊天区字体
    QColor  m_textColor;        // 自己发送的文字颜色（无效 = 默认黑）
    int     m_maxMsgLength = 255;   // 单条文字消息长度上限

    static const int kMaxImageSide = 1024;   // 图片发送前的最长边（超过则等比缩小）
    static const int kMaxImageBytes = 600 * 1024; // 压缩后的目标上限
    static const int kMaxAudioSec = 60;      // 单条语音最长 60 秒
    static const qint64 kAckTimeoutMs = 10 * 1000;   // 等服务端回执的时限

    // 待确认的消息：key -> 原始报文（超时重发用） / 超时时刻
    QHash<QString, QJsonObject> m_pendingPayloads;
    QHash<QString, qint64>      m_pendingDeadline;
    QTimer *m_ackTimer = nullptr;
};

#endif // CHATWINDOW_H

#ifndef TCPSERVER_H
#define TCPSERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QList>
#include <QVector>
#include <QThreadPool>
#include <QMutex>
#include <QTimer>

#include "clientsocket.h"
class FileSocket;

// 服务器管理类
class MyServer : public QObject {
    Q_OBJECT
public:
    explicit MyServer(QObject *parent = nullptr);
    ~MyServer();

    bool StartListen(int port = 6666);
    void CloseListen();
    bool isListening() const;       // 是否正在监听
    int  listenPort() const;        // 当前监听端口（未监听返回 -1）
signals:
    void signalUserStatus(const QString &text);
protected:
    QTcpServer *m_tcpServer;


public slots:

protected slots:
    // 继承虚函数
    virtual void SltNewConnection() = 0;
    virtual void SltConnected() = 0;
    virtual void SltDisConnected() = 0;
};

// 消息服务器
class TcpMsgServer : public MyServer
{
    Q_OBJECT
public:
    explicit TcpMsgServer(QObject *parent = 0);
    ~TcpMsgServer();

    void insertMessageQueue(const int &send,const int &getId, const QJsonValue &jsonVal, const quint8 &type);
    void sendUserMessageQueue(const QString &userId);   //用户登录成功后服务器检索消息队列转发属于他的消息

    int  onlineCount() const;      // 已登录用户数
    int  socketCount() const;      // 已连接的 socket 数
    void reloadGroupCache();       // 还原数据库后重新加载群成员缓存

    // 在线状态的唯一真相是连接表，不是数据库里的 status 字段
    bool isUserOnline(int userId) const;
    static TcpMsgServer *instance();   // 供 ClientSocket 在登录时查询真实在线状态
    void sweepIdleClients();           // 清理心跳超时的连接（异常掉线的兜底回收）

signals:
    void signalDownloadFile(const QJsonValue &json);
    void signalServerLog(const QString &text);              // 服务器运行日志（供管理界面显示）
    void signalClientChanged(int online, int sockets);      // 在线/连接数变化

private:
    // 客户端管理
    QVector < ClientSocket * > m_clients;   //广播快
    QHash<QString, ClientSocket*> m_clientHash;  //单播快
    QHash<int, QSet<int>> m_groupMembersCache;  //群号 -> 群成员
    QTimer *m_heartbeatTimer = nullptr;          // 心跳超时扫描
    static TcpMsgServer *s_instance;
public slots:
    void SltTransFileToClient(const int &userId, const QJsonValue &jsonVal);

private slots:
    void SltNewConnection();
    void SltConnected();
    void SltDisConnected();
    void SltPublicMsgToClient(const int &id, const quint8 &type, const QJsonValue &json); //广播
    //id发给targetId
    void SltPrivateMsgToClient(const int &id, const int &targetId, const quint8 &type, const QJsonValue &json);    //单播
    void SltGroupMsgToClient(const int &id, const int &groupId, const quint8 &type, const QJsonValue &json);    //组播

    void SltLoginSuccess(ClientSocket *client, const QString &userId);
    void SltAddGroupMembers(const int &groupId, const int &userId);
    void SltRemoveGroupMembers(const int &groupId, const int &userId);
    // 头像变更广播（通知除改头像的人之外的所有在线用户）
    void SltHeadChangedToClient(const int &id, const QJsonValue &json);
};


// 文件服务器
class TcpFileServer : public QTcpServer
{
    Q_OBJECT
public:
    explicit TcpFileServer(QObject *parent = nullptr);
    ~TcpFileServer() override;

    // 启动服务器
    bool StartListen(quint16 port);
    // 停止服务器
    void stop();
    // 设置文件保存根目录
    void setFileSaveDir(const QString &dir);
    // 设置线程池最大线程数
    void setMaxThreadCount(int count);

signals:
    // 新客户端连接
    void clientConnected(const QString &clientKey);
    // 文件接收完成
    void fileRecvFinished(const QString &clientKey, const QString &filePath);
    // 任务失败
    void taskFailed(const QString &clientKey, const QString &error);
    // 客户端断开连接
    void clientDisconnected(const QString &clientKey);

protected:
    // 重写QTcpServer的新连接处理
    void incomingConnection(qintptr socketDescriptor) override;

private slots:
    // 接收FileSocket的信号
    void onFileRecvFinished(const QString &clientKey, const QString &filePath);
    void onTaskFailed(const QString &clientKey, const QString &error);
    void onClientDisconnected(const QString &clientKey);

private:
    QThreadPool *m_threadPool;              // 自定义线程池
    QMutex m_clientMutex;                   // 客户端列表锁
    QHash<QString, FileSocket*> m_clientMap;// 客户端标识
    QString m_fileSaveDir;                  // 文件保存根目录
    bool m_isRunning = false;               // 服务器是否运行中
};

#endif // TCPSERVER_H

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QDateTime>
#include <QVariantMap>

#include "netdb/myserver.h"
#include "basewidget/customwidget.h"

class QButtonGroup;
class QTimer;
class QMenu;
class QAction;
class QStandardItemModel;
class QListWidget;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTextBrowser;
class QPushButton;
class QCheckBox;
class QTableWidget;

namespace Ui {
class MainWindow;
}

class MainWindow : public CustomMoveWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    // ===== 窗口 / 导航 / 登录 =====
    void sltButtonClicked(int index);
    void on_btnQuit_clicked();
    void on_btnWinClose_clicked();
    void on_btnWinMin_clicked();
    void on_btnExit_clicked();
    void on_btnLogin_clicked();

    // ===== 用户管理 =====
    void on_btnUserInsert_clicked();
    void on_btnUserRefresh_clicked();
    void on_btnUserEdit_clicked();
    void on_btnUserDelete_clicked();
    void sltUserTableClicked(const QModelIndex &index);
    void sltChooseHead();

    // ===== 用户查询 =====
    void sltSearchUser();

    // ===== 服务配置 =====
    void sltApplyServerConfig();
    void sltToggleServer();

    // ===== 密码修改 =====
    void on_btnPasswdChange_clicked();

    // ===== 数据备份 =====
    void sltChooseBackupDir();
    void on_btnBackup_clicked();
    void on_btnDataUdo_clicked();

    // ===== 消息审计（端到端加密）=====
    void refreshAuditTable();

    // ===== 托盘 =====
    void sltTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void sltTrayAction(QAction *action);

    // ===== 服务器事件 =====
    void sltServerLog(const QString &text);
    void sltClientChanged(int online, int sockets);
    void sltTick();

private:
    void initTray();               // 托盘图标 + 菜单
    void initClock();              // 状态栏时钟
    void initUserTable();          // 用户管理表格
    void initSearchPage();         // 用户查询页
    void initServerPage();         // 服务配置页
    void initBackupPage();         // 数据备份页
    void initPasswordPage();       // 密码修改页
    void initAuditPage();          // 消息审计页（端到端加密：只看密文与元数据）

    void appendLog(const QString &text, const QString &color = QStringLiteral("#333333"));
    void showLoginTip(const QString &text);
    void notifyTray(const QString &title, const QString &msg, bool warn = false,
                    const QString &logColor = QStringLiteral("#1a7f37"));
    void refreshUserTable();
    void fillUserForm(const QVariantMap &user);
    void clearUserForm();
    void showUserDetail(const QString &key);
    void refreshBackupList();
    void refreshRuntimeInfo();
    void updateTrayTip();
    void showMainWindow(int page = -1);
    void hideToTray();              // 隐藏到托盘（首次给出气泡提示）
    bool isWindowShowing() const;   // 窗口是否处于“已显示且未最小化”

    bool backupDatabase(const QString &filePath, QString *err = nullptr);
    bool restoreDatabase(const QString &filePath, QString *err = nullptr);

    void loadConfig();
    void saveConfig();

    static QString saveHeadImage(const QString &srcFile, int userId);

private:
    Ui::MainWindow *ui;

    QButtonGroup  *m_buttonGroup;

    TcpMsgServer  *m_tcpServer;
    TcpFileServer *m_tcpFileServer;

    // 托盘
    QSystemTrayIcon *m_tray;
    QMenu           *m_trayMenu;
    bool             m_bQuit;          // 真正退出程序的标志
    bool             m_bTrayTipShown;  // 是否已提示过"最小化到托盘"
    bool             m_bLoggedIn;
    QString          m_strLoginUser;
    qint64           m_msLastTrayClick;   // 上次单击托盘的时间，用于去掉双击的重复触发

    // 状态栏
    QTimer *m_timer;
    QDateTime m_dtStart;
    QString   m_strHostIp;

    // 用户管理
    QStandardItemModel *m_userModel;
    int     m_nSelectedUserId;
    QString m_strSelectedHead;   // 表单里待保存的头像文件名

    // 用户查询
    QLineEdit    *m_lineSearch;
    QTextBrowser *m_textUserInfo;

    // 服务配置
    QSpinBox    *m_spinMsgPort;
    QSpinBox    *m_spinFilePort;
    QLabel      *m_labelState;
    QLabel      *m_labelOnline;
    QLabel      *m_labelSocket;
    QLabel      *m_labelRuntime;
    QLabel      *m_labelDbSize;
    QPushButton *m_btnServerToggle;

    // 登录页提示
    QLabel *m_loginTip;

    // 数据备份
    QListWidget *m_backupList;
    QString      m_strBackupDir;

    // 消息审计（服务器零知识：只展示密文与收发元数据，无法还原正文）
    QWidget      *m_auditPage;
    QPushButton  *m_btnAuditPage;
    QPushButton  *m_btnMsgRefresh;
    QLabel       *m_labelMsgInfo;
    QTableWidget *m_msgTable;
    int           m_nAuditPageIndex;

    // 端口配置
    int m_nMsgPort;
    int m_nFilePort;
};

#endif // MAINWINDOW_H

#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardItemModel>
#include <QTableView>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "netdb/databasemag.h"
#include "comapi/myapp.h"
#include "comapi/global.h"
#include "comapi/cryptoutil.h"
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// 统一的提示框（按钮文字用中文，避免默认的 Yes/No/OK）
void showMsg(QWidget *parent, QMessageBox::Icon icon,
             const QString &title, const QString &text)
{
    QMessageBox box(icon, title, text, QMessageBox::NoButton, parent);
    box.addButton(QStringLiteral("知道了"), QMessageBox::AcceptRole);
    box.exec();
}

bool askYesNo(QWidget *parent, const QString &title, const QString &text,
              const QString &yesText = QStringLiteral("确定"),
              const QString &noText = QStringLiteral("取消"))
{
    QMessageBox box(QMessageBox::Question, title, text, QMessageBox::NoButton, parent);
    QAbstractButton *yes = box.addButton(yesText, QMessageBox::AcceptRole);
    box.addButton(noText, QMessageBox::RejectRole);
    box.setDefaultButton(qobject_cast<QPushButton *>(yes));
    box.exec();
    return box.clickedButton() == yes;
}

// 文件大小显示
QString sizeText(qint64 bytes)
{
    return myHelper::CalcSize(bytes);
}

} // namespace

MainWindow::MainWindow(QWidget *parent) :
    CustomMoveWidget(parent),
    ui(new Ui::MainWindow),
    m_buttonGroup(nullptr),
    m_tcpServer(nullptr),
    m_tcpFileServer(nullptr),
    m_tray(nullptr),
    m_trayMenu(nullptr),
    m_bQuit(false),
    m_bTrayTipShown(false),
    m_bLoggedIn(false),
    m_msLastTrayClick(0),
    m_timer(nullptr),
    m_userModel(nullptr),
    m_nSelectedUserId(-1),
    m_lineSearch(nullptr),
    m_textUserInfo(nullptr),
    m_spinMsgPort(nullptr),
    m_spinFilePort(nullptr),
    m_labelState(nullptr),
    m_labelOnline(nullptr),
    m_labelSocket(nullptr),
    m_labelRuntime(nullptr),
    m_labelDbSize(nullptr),
    m_btnServerToggle(nullptr),
    m_loginTip(nullptr),
    m_backupList(nullptr),
    m_auditPage(nullptr),
    m_btnAuditPage(nullptr),
    m_btnMsgRefresh(nullptr),
    m_labelMsgInfo(nullptr),
    m_msgTable(nullptr),
    m_nAuditPageIndex(-1),
    m_nMsgPort(60101),
    m_nFilePort(60102)
{
    ui->setupUi(this);
    // 无边框窗口默认是 WS_POPUP 且不带 WS_MINIMIZEBOX，Windows 会认为它不可最小化，
    // 点任务栏图标毫无反应。这里显式声明“可最小化 + 系统菜单”，
    // 才能在任务栏图标上单击最小化/还原（界面依旧无边框，由自绘标题栏控制）。
    setWindowFlags(Qt::FramelessWindowHint
                   | Qt::WindowMinimizeButtonHint
                   | Qt::WindowSystemMenuHint);
    setQss(QString(":/qss/resource/qss/default.css"));

    MyApp::InitApp(".");
    // 头像目录里必须有默认头像，缺了就现场生成一张
    MyApp::EnsureDefaultHead();

    loadConfig();
    m_strHostIp = myHelper::GetIP();
    m_dtStart = QDateTime::currentDateTime();

    // ===== 页面初始化（必须在打开数据库之前把控件建好）=====
    initUserTable();
    initSearchPage();
    initServerPage();
    initBackupPage();
    initPasswordPage();
    initAuditPage();

    // ===== 左侧导航 =====
    m_buttonGroup = new QButtonGroup(this);
    m_buttonGroup->setExclusive(true);
    m_buttonGroup->addButton(ui->btnUserInfoPage, 0);      // 用户查询
    m_buttonGroup->addButton(ui->btnScorePage, 1);         // 用户信息（运行日志）
    m_buttonGroup->addButton(ui->btnScoreInsertPage, 2);   // 服务配置
    m_buttonGroup->addButton(ui->btnUsersPage, 3);         // 用户管理
    m_buttonGroup->addButton(ui->btnPasswordPage, 4);      // 密码修改
    m_buttonGroup->addButton(ui->btnDataBackup, 5);        // 数据备份
    if (m_btnAuditPage) m_buttonGroup->addButton(m_btnAuditPage, m_nAuditPageIndex);  // 消息审计
    connect(m_buttonGroup, SIGNAL(buttonClicked(int)), this, SLOT(sltButtonClicked(int)));

    ui->stackedWidgetFunc->setCurrentIndex(0);
    ui->stackedWidgetMain->setCurrentIndex(0);   // 启动先显示登录页
    ui->btnUserInfoPage->setChecked(true);

    // ===== 数据库 =====
    if (!DataBaseMag::getInstance()->openDatabase(QString("%1server.db").arg(MyApp::m_strDatabasePath))) {
        appendLog("数据库打开失败，请检查 Data/Database 目录权限！", QStringLiteral("#c62828"));
    } else {
        // 端到端加密：服务器不生成、不保存任何私钥。
        // 它只保管用户公钥（公开信息），因此永远无法解密消息正文。
        appendLog("端到端加密已启用：服务器零知识，仅转发密文");
    }

    // ===== 消息服务 =====
    m_tcpServer = new TcpMsgServer(this);
    connect(m_tcpServer, &TcpMsgServer::signalServerLog, this, &MainWindow::sltServerLog);
    connect(m_tcpServer, &TcpMsgServer::signalClientChanged, this, &MainWindow::sltClientChanged);

    bool msgListenOk = m_tcpServer->StartListen(m_nMsgPort);
    if (!msgListenOk) {
        appendLog(QString("消息端口 %1 监听失败（可能被其它程序占用）").arg(m_nMsgPort),
                  QStringLiteral("#c62828"));
    }

    // ===== 文件服务 =====
    m_tcpFileServer = new TcpFileServer(this);
    if (!m_tcpFileServer->StartListen(static_cast<quint16>(m_nFilePort))) {
        appendLog(QString("文件端口 %1 监听失败（可能被其它程序占用）").arg(m_nFilePort),
                  QStringLiteral("#c62828"));
    }

    // ===== 托盘 & 状态栏 =====
    initTray();
    initClock();
    refreshRuntimeInfo();
    refreshBackupList();
    updateTrayTip();

    if (!msgListenOk) {
        notifyTray("消息服务启动失败",
                   QString("端口 %1 无法监听，请到「服务配置」修改端口。").arg(m_nMsgPort),
                   true, QStringLiteral("#c62828"));
    }

    // ===== 登录页提示 =====
    m_loginTip = new QLabel(ui->LoginWidget);
    m_loginTip->setGeometry(610, 302, 282, 24);
    m_loginTip->setAlignment(Qt::AlignCenter);
    m_loginTip->setStyleSheet("color:#C62828; background:transparent;");
    m_loginTip->hide();

    connect(ui->lineEditUser, &QLineEdit::returnPressed, this, &MainWindow::on_btnLogin_clicked);
    connect(ui->lineEditPasswd, &QLineEdit::returnPressed, this, &MainWindow::on_btnLogin_clicked);
    ui->lineEditUser->setFocus();

    // ===== 启动日志 =====
    appendLog("==============================================", QStringLiteral("#387ECC"));
    appendLog("局域网通信服务器已启动", QStringLiteral("#387ECC"));
    appendLog(QString("本机 IP：%1").arg(m_strHostIp));
    appendLog(QString("消息服务端口：%1    文件服务端口：%2").arg(m_nMsgPort).arg(m_nFilePort));
    appendLog(QString("数据库：%1").arg(QDir::toNativeSeparators(MyApp::m_strDatabasePath + "server.db")));
    appendLog(QString("备份目录：%1").arg(QDir::toNativeSeparators(m_strBackupDir)));
    appendLog("==============================================", QStringLiteral("#387ECC"));
}

MainWindow::~MainWindow()
{
    if (m_timer) {
        m_timer->stop();
    }
    DataBaseMag::getInstance()->changeAllUserStatus();
    DataBaseMag::getInstance()->closeDatabase();
    delete ui;
}

// ============================ 托盘 ============================

void MainWindow::initTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qWarning() << "[托盘] 当前系统不支持系统托盘";
    }

    m_tray = new QSystemTrayIcon(QIcon(":/resource/images/ic_app.png"), this);
    m_tray->setToolTip("局域网通信服务器");

    m_trayMenu = new QMenu(this);
    QAction *actShow = m_trayMenu->addAction("显示主界面");
    actShow->setData(-1);
    QAction *actUsers = m_trayMenu->addAction("用户管理");
    actUsers->setData(3);
    QAction *actBackup = m_trayMenu->addAction("数据备份");
    actBackup->setData(5);
    m_trayMenu->addSeparator();
    QAction *actQuit = m_trayMenu->addAction("退出系统");
    actQuit->setData(-99);
    m_tray->setContextMenu(m_trayMenu);

    connect(m_trayMenu, &QMenu::triggered, this, &MainWindow::sltTrayAction);
    connect(m_tray, &QSystemTrayIcon::activated, this, &MainWindow::sltTrayActivated);

    m_tray->show();
}

void MainWindow::sltTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (m_bQuit) return;

    switch (reason) {
    case QSystemTrayIcon::Trigger: {
        // 单击托盘图标：纯切换——窗口显示着就收起，收起/最小化了就显示
        m_msLastTrayClick = QDateTime::currentMSecsSinceEpoch();
        if (isWindowShowing()) {
            hideToTray();
        } else {
            showMainWindow();
        }
        break;
    }
    case QSystemTrayIcon::DoubleClick:
        // Windows 上双击会先来一次 Trigger，紧接着再来 DoubleClick。
        // 刚处理过单击时忽略这次双击，避免“先隐藏又弹出”的闪烁。
        if (QDateTime::currentMSecsSinceEpoch() - m_msLastTrayClick > 500) {
            if (!isWindowShowing()) {
                showMainWindow();
            }
        }
        break;
    case QSystemTrayIcon::MiddleClick:
        showMainWindow();
        break;
    default:
        break;
    }
}

void MainWindow::sltTrayAction(QAction *action)
{
    if (!action) return;
    const int value = action->data().toInt();
    if (value == -99) {            // 退出系统
        if (m_tcpServer && m_tcpServer->onlineCount() > 0) {
            if (!askYesNo(this, "退出系统",
                          QString("当前还有 %1 位用户在线，退出后服务器将停止服务。\n确定退出吗？")
                          .arg(m_tcpServer->onlineCount()),
                          "退出", "取消")) {
                return;
            }
        }
        m_bQuit = true;
        if (m_tray) m_tray->hide();
        qApp->quit();
        return;
    }
    showMainWindow(value < 0 ? -1 : value);
}

void MainWindow::showMainWindow(int page)
{
    if (page >= 0) {
        if (page >= ui->stackedWidgetFunc->count()) page = 0;
        ui->stackedWidgetFunc->setCurrentIndex(page);
        if (QAbstractButton *btn = m_buttonGroup->button(page)) {
            btn->setChecked(true);
        }
        sltButtonClicked(page);
    }
    showNormal();      // 最小化/隐藏状态都会还原
    raise();
    activateWindow();
}

bool MainWindow::isWindowShowing() const
{
    return isVisible() && !isMinimized();
}

void MainWindow::hideToTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable() || !m_tray || !m_tray->isVisible()) {
        showMinimized();   // 没有托盘就退化成普通最小化
        return;
    }

    hide();

    if (!m_bTrayTipShown) {
        m_tray->showMessage("服务器仍在后台运行",
                            QString("程序已最小化到托盘，消息/文件服务继续监听（%1 / %2）。\n单击托盘图标可再次显示窗口。")
                            .arg(m_nMsgPort).arg(m_nFilePort),
                            QSystemTrayIcon::Information, 4000);
        m_bTrayTipShown = true;
    } else {
        appendLog(QString("窗口已最小化到托盘，服务器继续运行（在线 %1 人）")
                  .arg(m_tcpServer ? m_tcpServer->onlineCount() : 0));
    }
}

void MainWindow::updateTrayTip()
{
    if (!m_tray) return;
    m_tray->setToolTip(QString("局域网通信服务器\n消息端口 %1 · 文件端口 %2\n在线用户 %3 人")
                       .arg(m_nMsgPort)
                       .arg(m_nFilePort)
                       .arg(m_tcpServer ? m_tcpServer->onlineCount() : 0));
}

void MainWindow::notifyTray(const QString &title, const QString &msg, bool warn,
                            const QString &logColor)
{
    appendLog(QString("%1：%2").arg(title, msg), logColor);
    if (m_tray && m_tray->isVisible() && (!isVisible() || isMinimized())) {
        m_tray->showMessage(title, msg,
                            warn ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information,
                            3000);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_bQuit) {
        event->accept();
        return;
    }

    // 系统没有托盘时无法恢复窗口，直接退出
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        m_bQuit = true;
        event->accept();
        return;
    }

    // 关闭窗口 = 最小化到托盘，服务继续运行
    event->ignore();
    hideToTray();
}

void MainWindow::on_btnWinClose_clicked()
{
    close();   // 走 closeEvent，隐藏到托盘
}

void MainWindow::on_btnWinMin_clicked()
{
    // 最小化到 Windows 任务栏（任务栏图标单击可再还原）；
    // 关闭按钮才是收进托盘，两者互不冲突。
    showMinimized();
}

void MainWindow::on_btnQuit_clicked()
{
    if (m_tcpServer && m_tcpServer->onlineCount() > 0) {
        if (!askYesNo(this, "退出系统",
                      QString("当前还有 %1 位用户在线，退出后服务器将停止服务。\n确定退出吗？")
                      .arg(m_tcpServer->onlineCount()),
                      "退出", "取消")) {
            return;
        }
    } else if (!askYesNo(this, "退出系统", "确定要退出服务器管理程序吗？", "退出", "取消")) {
        return;
    }

    m_bQuit = true;
    if (m_tray) m_tray->hide();
    qApp->quit();
}

void MainWindow::on_btnExit_clicked()
{
    on_btnQuit_clicked();   // 登录页的「退出」
}

// ============================ 状态栏 ============================

void MainWindow::initClock()
{
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::sltTick);
    m_timer->start();
    sltTick();
}

void MainWindow::sltTick()
{
    const QDateTime now = QDateTime::currentDateTime();
    ui->labelSystemTime->setText(now.toString("yyyy-MM-dd HH:mm:ss  dddd"));
    ui->labelHostAddr->setText(QString("本机IP：%1    消息：%2    文件：%3    在线：%4 人")
                               .arg(m_strHostIp)
                               .arg(m_nMsgPort)
                               .arg(m_nFilePort)
                               .arg(m_tcpServer ? m_tcpServer->onlineCount() : 0));
    refreshRuntimeInfo();
}

void MainWindow::refreshRuntimeInfo()
{
    if (!m_labelState) return;

    const int online = m_tcpServer ? m_tcpServer->onlineCount() : 0;
    const int sockets = m_tcpServer ? m_tcpServer->socketCount() : 0;
    const bool listening = m_tcpServer && m_tcpServer->isListening();

    m_labelState->setText(listening
                          ? QString("监听中 · 端口 %1").arg(m_tcpServer->listenPort())
                          : QStringLiteral("已停止"));
    m_labelState->setStyleSheet(listening ? "color:#1a7f37;font-weight:bold;"
                                          : "color:#c62828;font-weight:bold;");
    m_labelOnline->setText(QString("%1 人").arg(online));
    m_labelSocket->setText(QString("%1 个").arg(sockets));

    if (m_btnServerToggle) {
        m_btnServerToggle->setText(listening ? "停止服务" : "启动服务");
    }

    // 运行时长
    qint64 secs = m_dtStart.secsTo(QDateTime::currentDateTime());
    if (secs < 0) secs = 0;
    const qint64 h = secs / 3600;
    const qint64 m = (secs % 3600) / 60;
    const qint64 s = secs % 60;
    m_labelRuntime->setText(QString("%1:%2:%3")
                            .arg(h, 2, 10, QChar('0'))
                            .arg(m, 2, 10, QChar('0'))
                            .arg(s, 2, 10, QChar('0')));

    // 数据库大小
    const QFileInfo dbInfo(MyApp::m_strDatabasePath + "server.db");
    m_labelDbSize->setText(sizeText(dbInfo.exists() ? dbInfo.size() : 0));
}

void MainWindow::appendLog(const QString &text, const QString &color)
{
    if (!ui || !ui->textBrowser) return;
    const QString html = QString("<div style='color:%1;'>[%2] %3</div>")
            .arg(color,
                 QDateTime::currentDateTime().toString("HH:mm:ss"),
                 text.toHtmlEscaped());
    ui->textBrowser->append(html);
}

void MainWindow::sltServerLog(const QString &text)
{
    appendLog(text, QStringLiteral("#444444"));
    updateTrayTip();
}

void MainWindow::sltClientChanged(int online, int sockets)
{
    Q_UNUSED(online)
    Q_UNUSED(sockets)
    updateTrayTip();
    refreshRuntimeInfo();
    if (m_bLoggedIn) {
        refreshUserTable();   // 在线状态列实时变化
    }
}

// ============================ 导航 ============================

void MainWindow::sltButtonClicked(int index)
{
    if (index < 0 || index >= ui->stackedWidgetFunc->count()) return;
    ui->stackedWidgetFunc->setCurrentIndex(index);

    switch (index) {
    case 0:   // 用户查询
        if (m_bLoggedIn) sltSearchUser();
        break;
    case 3:   // 用户管理
        if (m_bLoggedIn) refreshUserTable();
        break;
    case 4:   // 密码修改
        ui->lineEditOldPasswd->clear();
        ui->lineEditNewPasswd->clear();
        ui->lineEditNewPasswd2->clear();
        break;
    case 5:   // 数据备份
        refreshBackupList();
        break;
    default:
        if (index == m_nAuditPageIndex) {   // 消息审计
            refreshAuditTable();
        }
        break;
    }
    refreshRuntimeInfo();
}

// ============================ 登录 ============================

void MainWindow::showLoginTip(const QString &text)
{
    if (!m_loginTip) return;
    m_loginTip->setText(text);
    m_loginTip->setVisible(!text.isEmpty());
}

void MainWindow::on_btnLogin_clicked()
{
    const QString name = ui->lineEditUser->text().trimmed();
    const QString passwd = ui->lineEditPasswd->text();

    if (name.isEmpty() || passwd.isEmpty()) {
        showLoginTip("请输入用户名和密码");
        return;
    }

    // 服务器管理登录只做校验，不修改 USERINFO.status（那是客户端在线状态）
    if (!DataBaseMag::getInstance()->checkPassword(name, passwd)) {
        showLoginTip(DataBaseMag::getInstance()->haveUser(name)
                     ? "密码错误，请重新输入"
                     : "用户名或密码错误");
        appendLog(QString("登录失败：%1 用户名或密码错误").arg(name), QStringLiteral("#c62828"));
        return;
    }

    const QVariantMap user = DataBaseMag::getInstance()->getUserInfoByName(name);
    if (user.isEmpty()) {
        showLoginTip("用户名或密码错误");
        return;
    }

    m_bLoggedIn = true;
    m_strLoginUser = user.value("name").toString();
    MyApp::m_strUserName = m_strLoginUser;
    MyApp::m_nId = user.value("id").toInt();

    showLoginTip(QString());
    ui->lineEditPasswd->clear();
    ui->stackedWidgetMain->setCurrentIndex(1);

    refreshUserTable();
    sltSearchUser();
    refreshBackupList();
    refreshAuditTable();
    refreshRuntimeInfo();

    appendLog(QString("管理员 %1（ID:%2）登录成功").arg(m_strLoginUser).arg(MyApp::m_nId),
              QStringLiteral("#1a7f37"));
    notifyTray("登录成功", QString("管理员 %1 已登录服务器").arg(m_strLoginUser));
    ui->textBrowser->moveCursor(QTextCursor::End);
}

// ============================ 用户查询 ============================

void MainWindow::initSearchPage()
{
    ui->groupBox_2->setTitle("用户查询");
    ui->groupBox_2->setMinimumSize(0, 0);
    ui->groupBox_2->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    if (QLayoutItem *item = ui->verticalLayout_11->itemAt(0)) {
        item->setAlignment(Qt::Alignment());   // 取消左上角对齐，让查询页铺满
    }

    QHBoxLayout *searchRow = new QHBoxLayout;
    QLabel *label = new QLabel("关键字：");
    m_lineSearch = new QLineEdit;
    m_lineSearch->setPlaceholderText("输入用户名或用户ID后回车；留空查看服务器概况");
    QPushButton *btnSearch = new QPushButton("查询");
    btnSearch->setObjectName("btnUserSearch");
    searchRow->addWidget(label);
    searchRow->addWidget(m_lineSearch, 1);
    searchRow->addWidget(btnSearch);
    ui->verticalLayout_4->addLayout(searchRow);

    m_textUserInfo = new QTextBrowser;
    m_textUserInfo->setOpenExternalLinks(false);
    m_textUserInfo->setStyleSheet("QTextBrowser { border:1px solid #D5DCE5; background:#FFFFFF; }");
    ui->verticalLayout_4->addWidget(m_textUserInfo, 1);

    connect(btnSearch, &QPushButton::clicked, this, &MainWindow::sltSearchUser);
    connect(m_lineSearch, &QLineEdit::returnPressed, this, &MainWindow::sltSearchUser);
}

void MainWindow::sltSearchUser()
{
    if (!m_textUserInfo) return;
    if (!m_bLoggedIn) {
        m_textUserInfo->setHtml("<p style='color:#888888;'>请先登录服务器管理账号。</p>");
        return;
    }

    const QString key = m_lineSearch ? m_lineSearch->text().trimmed() : QString();

    // 空关键字：显示服务器概况
    if (key.isEmpty()) {
        const int total = DataBaseMag::getInstance()->getUserCount();
        const int online = m_tcpServer ? m_tcpServer->onlineCount() : 0;
        QString html;
        html += "<div style='font-family:微软雅黑;font-size:13px;color:#333333;'>";
        html += "<h3 style='color:#387ECC;'>服务器概况</h3>";
        html += "<table cellspacing='0' cellpadding='5'>";
        html += QString("<tr><td>用户总数：</td><td><b>%1</b> 人</td></tr>").arg(total);
        html += QString("<tr><td>在线客户端：</td><td><b style='color:#1a7f37;'>%1</b> 人</td></tr>").arg(online);
        html += QString("<tr><td>消息端口：</td><td>%1</td></tr>").arg(m_nMsgPort);
        html += QString("<tr><td>文件端口：</td><td>%1</td></tr>").arg(m_nFilePort);
        html += QString("<tr><td>启动时间：</td><td>%1</td></tr>")
                .arg(m_dtStart.toString("yyyy-MM-dd HH:mm:ss"));
        html += QString("<tr><td>数据库：</td><td>%1</td></tr>")
                .arg(QDir::toNativeSeparators(MyApp::m_strDatabasePath + "server.db"));
        html += "</table>";
        html += "<p style='color:#999999;'>在左侧输入用户名或用户ID，可查看单个用户的详细信息。</p>";
        html += "</div>";
        m_textUserInfo->setHtml(html);
        return;
    }

    const QVariantMap user = DataBaseMag::getInstance()->getUserInfoByName(key);
    if (user.isEmpty()) {
        m_textUserInfo->setHtml(QString("<p style='color:#c62828;font-family:微软雅黑;'>"
                                        "没有找到匹配「%1」的用户。</p>")
                                .arg(key.toHtmlEscaped()));
        return;
    }

    const int userId = user.value("id").toInt();
    const QString userName = user.value("name").toString();
    // 右侧详情也以连接表为准，避免和用户管理表格显示不一致
    bool isOnline = user.value("status").toInt() == OnLine;
    if (m_tcpServer) {
        const bool live = m_tcpServer->isUserOnline(userId);
        if (live != isOnline) {
            DataBaseMag::getInstance()->UpdateUserStatus(userId, live ? OnLine : OffLine, false);
            isOnline = live;
        }
    }
    const QString headPath = MyApp::HeadFullPath(user.value("head").toString());
    const QString imgSrc = QUrl::fromLocalFile(headPath).toString();
    const QString lastTime = user.value("lasttime").toString().isEmpty()
            ? QStringLiteral("-") : user.value("lasttime").toString();
    const int friendCount = DataBaseMag::getInstance()->getFriendCount(userId);
    const int groupCount = DataBaseMag::getInstance()->getGroupCount(userId);

    QString html;
    html += "<div style='font-family:微软雅黑;font-size:13px;color:#333333;'>";
    html += "<table cellspacing='0' cellpadding='6'>";
    html += QString("<tr><td rowspan='6' width='104' valign='top'>"
                    "<img src='%1' width='88' height='88'></td>").arg(imgSrc);
    html += QString("<td><span style='font-size:17px;font-weight:bold;color:#387ECC;'>%1</span>"
                    "&nbsp;&nbsp;<span style='color:%2;'>● %3</span></td></tr>")
            .arg(userName.toHtmlEscaped(),
                 isOnline ? QStringLiteral("#1a7f37") : QStringLiteral("#999999"),
                 isOnline ? QStringLiteral("在线") : QStringLiteral("离线"));
    html += QString("<tr><td>用户ID：%1</td></tr>").arg(userId);
    html += QString("<tr><td>联系电话：%1</td></tr>")
            .arg(user.value("phone").toString().isEmpty() ? "-" : user.value("phone").toString().toHtmlEscaped());
    html += QString("<tr><td>工作组：%1</td></tr>")
            .arg(user.value("work_group").toString().isEmpty() ? "-" : user.value("work_group").toString().toHtmlEscaped());
    html += QString("<tr><td>工作邮箱：%1</td></tr>")
            .arg(user.value("email").toString().isEmpty() ? "-" : user.value("email").toString().toHtmlEscaped());
    html += QString("<tr><td>最后登录：%1</td></tr>").arg(lastTime.toHtmlEscaped());
    html += "</table>";
    html += QString("<hr><p style='color:#666666;'>好友 %1 位 · 已加入群组 %2 个</p>")
            .arg(friendCount).arg(groupCount);
    html += "</div>";
    m_textUserInfo->setHtml(html);
}

// ============================ 用户管理 ============================

void MainWindow::initUserTable()
{
    m_userModel = new QStandardItemModel(this);
    m_userModel->setHorizontalHeaderLabels(QStringList()
                                            << "ID" << "用户名" << "密码" << "头像"
                                            << "联系电话" << "工作组" << "工作邮箱"
                                            << "状态" << "最后登录");
    ui->tableViewUsers->setModel(m_userModel);
    ui->tableViewUsers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->tableViewUsers->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableViewUsers->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->tableViewUsers->setAlternatingRowColors(true);
    ui->tableViewUsers->setShowGrid(false);
    ui->tableViewUsers->verticalHeader()->setVisible(false);
    ui->tableViewUsers->horizontalHeader()->setHighlightSections(false);
    ui->tableViewUsers->horizontalHeader()->setStretchLastSection(true);
    // 按内容自适应列宽，最后一列拉伸填满，避免出现横向滚动条
    ui->tableViewUsers->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->tableViewUsers->setWordWrap(false);

    connect(ui->tableViewUsers, &QTableView::clicked,
            this, &MainWindow::sltUserTableClicked);

    ui->labelUserHead->installEventFilter(this);
    ui->labelUserHead->setCursor(Qt::PointingHandCursor);
    ui->labelUserHead->setToolTip("点击选择一张图片作为用户头像");
}

void MainWindow::refreshUserTable()
{
    if (!m_userModel) return;

    m_userModel->clear();
    m_userModel->setHorizontalHeaderLabels(QStringList()
                                            << "ID" << "用户名" << "密码" << "头像"
                                            << "联系电话" << "工作组" << "工作邮箱"
                                            << "状态" << "最后登录");

    const QList<QVariantMap> users = DataBaseMag::getInstance()->getAllUser();
    for (int i = 0; i < users.size(); ++i) {
        const QVariantMap &user = users.at(i);
        const int id = user.value("id").toInt();

        QStandardItem *idItem = new QStandardItem(QString::number(id));
        idItem->setData(id, Qt::UserRole);
        idItem->setTextAlignment(Qt::AlignCenter);

        QStandardItem *nameItem = new QStandardItem(user.value("name").toString());
        // 密码只存 PBKDF2 哈希，界面上不显示、也不回填，用掩码占位
        QStandardItem *pwdItem = new QStandardItem(QStringLiteral("••••••"));

        // 头像列显示缩略图，文件名没有信息量
        QStandardItem *headItem = new QStandardItem;
        QPixmap headPix(MyApp::HeadFullPath(user.value("head").toString()));
        if (headPix.isNull()) {
            headPix = QPixmap(":/resource/images/if_users.png");
        }
        headItem->setIcon(QIcon(headPix.scaled(28, 28, Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation)));
        headItem->setToolTip(QFileInfo(user.value("head").toString()).fileName());

        QStandardItem *phoneItem = new QStandardItem(user.value("phone").toString());
        QStandardItem *groupItem = new QStandardItem(user.value("work_group").toString());
        QStandardItem *emailItem = new QStandardItem(user.value("email").toString());

        // 在线状态以连接表为准，数据库里的 status 只是缓存（异常退出会留下脏数据）
        bool online = user.value("status").toInt() == OnLine;
        if (m_tcpServer) {
            const bool live = m_tcpServer->isUserOnline(id);
            if (live != online) {
                DataBaseMag::getInstance()->UpdateUserStatus(id, live ? OnLine : OffLine, false);
                online = live;
            }
        }
        QStandardItem *statusItem = new QStandardItem(online ? "在线" : "离线");
        statusItem->setTextAlignment(Qt::AlignCenter);
        statusItem->setForeground(online ? QColor("#1a7f37") : QColor("#888888"));

        const QString lastTime = user.value("lasttime").toString();
        QStandardItem *lastItem = new QStandardItem(lastTime.isEmpty() ? "-" : lastTime);

        QList<QStandardItem *> row;
        row << idItem << nameItem << pwdItem << headItem << phoneItem
            << groupItem << emailItem << statusItem << lastItem;
        m_userModel->appendRow(row);
    }

    if (users.isEmpty()) {
        QStandardItem *empty = new QStandardItem("暂无用户数据");
        empty->setForeground(QColor("#999999"));
        m_userModel->setItem(0, 0, empty);
    }

    // 中间区域宽度有限，密码列默认隐藏（选中用户后右侧表单里可以看到）
    ui->tableViewUsers->setColumnHidden(2, true);

    // 恢复之前选中的行，避免在线状态刷新时选中丢失
    if (m_nSelectedUserId > 0) {
        for (int row = 0; row < m_userModel->rowCount(); ++row) {
            QStandardItem *item = m_userModel->item(row, 0);
            if (item && item->data(Qt::UserRole).toInt() == m_nSelectedUserId) {
                ui->tableViewUsers->selectRow(row);
                break;
            }
        }
    }
}

void MainWindow::sltUserTableClicked(const QModelIndex &index)
{
    if (!index.isValid() || !m_userModel) return;
    QStandardItem *idItem = m_userModel->item(index.row(), 0);
    if (!idItem) return;

    const int id = idItem->data(Qt::UserRole).toInt();
    if (id <= 0) return;

    fillUserForm(DataBaseMag::getInstance()->getUserInfoById(id));
}

void MainWindow::fillUserForm(const QVariantMap &user)
{
    if (user.isEmpty()) return;

    m_nSelectedUserId = user.value("id").toInt();
    m_strSelectedHead = QFileInfo(user.value("head").toString()).fileName();

    ui->lineEditAddUser->setText(user.value("name").toString());
    // 密码是哈希，取不回原文：清空并提示"留空表示不修改"
    ui->lineEditAddPasswd->clear();
    ui->lineEditAddPasswd->setPlaceholderText(QStringLiteral("留空表示不修改密码"));
    ui->lineEditUserPhone->setText(user.value("phone").toString());
    ui->lineEditUserGroup->setText(user.value("work_group").toString());
    ui->lineEditUserEmail->setText(user.value("email").toString());

    QPixmap pix(MyApp::HeadFullPath(m_strSelectedHead));
    if (pix.isNull()) {
        pix = QPixmap(":/resource/images/if_users.png");
    }
    ui->labelUserHead->setPixmap(pix.scaled(66, 66, Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation));
}

void MainWindow::clearUserForm()
{
    m_nSelectedUserId = -1;
    m_strSelectedHead.clear();

    ui->lineEditAddUser->clear();
    ui->lineEditAddPasswd->clear();
    ui->lineEditAddPasswd->setPlaceholderText(QStringLiteral("请输入密码"));
    ui->lineEditUserPhone->clear();
    ui->lineEditUserGroup->clear();
    ui->lineEditUserEmail->clear();
    ui->labelUserHead->setPixmap(QPixmap(":/resource/images/if_users.png")
                                 .scaled(66, 66, Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation));
    ui->tableViewUsers->clearSelection();
}

void MainWindow::sltChooseHead()
{
    const QString file = QFileDialog::getOpenFileName(
                this, "选择头像图片", QDir::homePath(),
                "图片文件 (*.png *.jpg *.jpeg *.bmp)");
    if (file.isEmpty()) return;

    const QString name = saveHeadImage(file, m_nSelectedUserId);
    if (name.isEmpty()) {
        showMsg(this, QMessageBox::Warning, "头像",
                "图片读取失败，请换一张图片试试。");
        return;
    }

    m_strSelectedHead = name;
    QPixmap pix(MyApp::HeadFullPath(name));
    if (!pix.isNull()) {
        ui->labelUserHead->setPixmap(pix.scaled(66, 66, Qt::KeepAspectRatio,
                                                Qt::SmoothTransformation));
    }
    appendLog(QString("已选择头像：%1（保存后生效）").arg(name));
}

QString MainWindow::saveHeadImage(const QString &srcFile, int userId)
{
    QImage img(srcFile);
    if (img.isNull()) return QString();

    // 居中裁剪成正方形再缩放到 96x96
    const int side = qMin(img.width(), img.height());
    img = img.copy((img.width() - side) / 2, (img.height() - side) / 2, side, side);
    img = img.scaled(96, 96, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    const QString name = QString("head_%1_%2.png")
            .arg(userId > 0 ? userId : 0)
            .arg(QDateTime::currentMSecsSinceEpoch());
    const QString path = MyApp::m_strHeadPath + name;
    if (!img.save(path, "PNG")) return QString();
    return name;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->labelUserHead && event->type() == QEvent::MouseButtonRelease) {
        sltChooseHead();
        return true;
    }
    return CustomMoveWidget::eventFilter(watched, event);
}

void MainWindow::on_btnUserInsert_clicked()
{
    const QString name = ui->lineEditAddUser->text().trimmed();
    const QString passwd = ui->lineEditAddPasswd->text();
    if (name.isEmpty() || passwd.isEmpty()) {
        showMsg(this, QMessageBox::Warning, "添加用户", "用户名和密码不能为空。");
        return;
    }

    QString err;
    const int id = DataBaseMag::getInstance()->insertUser(
                name, passwd,
                ui->lineEditUserPhone->text().trimmed(),
                ui->lineEditUserGroup->text().trimmed(),
                ui->lineEditUserEmail->text().trimmed(),
                m_strSelectedHead, &err);
    if (id <= 0) {
        showMsg(this, QMessageBox::Warning, "添加用户失败", err);
        return;
    }

    appendLog(QString("添加用户成功：%1（ID:%2）").arg(name).arg(id),
              QStringLiteral("#1a7f37"));
    notifyTray("添加用户成功", QString("%1 已创建，ID 为 %2").arg(name).arg(id));
    refreshUserTable();
    clearUserForm();
    if (m_lineSearch) m_lineSearch->clear();
    sltSearchUser();
}

void MainWindow::on_btnUserRefresh_clicked()
{
    refreshUserTable();
    appendLog("用户列表已刷新");
}

void MainWindow::on_btnUserEdit_clicked()
{
    if (m_nSelectedUserId <= 0) {
        showMsg(this, QMessageBox::Information, "修改用户", "请先在左侧表格中选择要修改的用户。");
        return;
    }

    const QString name = ui->lineEditAddUser->text().trimmed();
    const QString passwd = ui->lineEditAddPasswd->text();
    if (name.isEmpty()) {
        showMsg(this, QMessageBox::Warning, "修改用户", "用户名不能为空。");
        return;
    }
    // 密码框留空 = 保持原密码（库里是哈希，无法回填原文）

    QString err;
    if (!DataBaseMag::getInstance()->updateUserInfo(
                m_nSelectedUserId, name, passwd,
                ui->lineEditUserPhone->text().trimmed(),
                ui->lineEditUserGroup->text().trimmed(),
                ui->lineEditUserEmail->text().trimmed(),
                m_strSelectedHead, &err)) {
        showMsg(this, QMessageBox::Warning, "修改用户失败", err);
        return;
    }

    appendLog(QString("修改用户成功：%1（ID:%2）").arg(name).arg(m_nSelectedUserId),
              QStringLiteral("#1a7f37"));
    notifyTray("修改成功", QString("用户 %1 的资料已更新").arg(name));
    refreshUserTable();
    fillUserForm(DataBaseMag::getInstance()->getUserInfoById(m_nSelectedUserId));
    sltSearchUser();
}

void MainWindow::on_btnUserDelete_clicked()
{
    if (m_nSelectedUserId <= 0) {
        showMsg(this, QMessageBox::Information, "删除用户", "请先在左侧表格中选择要删除的用户。");
        return;
    }

    const QVariantMap user = DataBaseMag::getInstance()->getUserInfoById(m_nSelectedUserId);
    const QString name = user.value("name").toString();
    if (!askYesNo(this, "删除用户",
                  QString("确定要删除用户「%1」吗？\n\n该用户的好友关系、群成员身份会一并删除，且不可恢复。")
                  .arg(name),
                  "删除", "取消")) {
        return;
    }

    QString err;
    if (!DataBaseMag::getInstance()->deleteUser(m_nSelectedUserId, &err)) {
        showMsg(this, QMessageBox::Warning, "删除失败", err);
        return;
    }

    appendLog(QString("已删除用户：%1").arg(name), QStringLiteral("#c62828"));
    notifyTray("删除成功", QString("用户 %1 已删除").arg(name), true, QStringLiteral("#c62828"));
    clearUserForm();
    refreshUserTable();
    if (m_lineSearch) m_lineSearch->clear();
    sltSearchUser();
}

// ============================ 服务配置 ============================

void MainWindow::initServerPage()
{
    QGroupBox *gbListen = new QGroupBox("服务监听", this);
    QGridLayout *grid = new QGridLayout(gbListen);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);

    m_spinMsgPort = new QSpinBox(gbListen);
    m_spinMsgPort->setRange(1024, 65535);
    m_spinMsgPort->setValue(m_nMsgPort);
    m_spinMsgPort->setMinimumWidth(120);

    m_spinFilePort = new QSpinBox(gbListen);
    m_spinFilePort->setRange(1024, 65535);
    m_spinFilePort->setValue(m_nFilePort);
    m_spinFilePort->setMinimumWidth(120);

    m_labelState = new QLabel("-", gbListen);
    m_labelOnline = new QLabel("0 人", gbListen);
    m_labelSocket = new QLabel("0 个", gbListen);

    m_btnServerToggle = new QPushButton("停止服务", gbListen);
    QPushButton *btnApply = new QPushButton("保存并应用", gbListen);

    grid->addWidget(new QLabel("消息服务端口：", gbListen), 0, 0);
    grid->addWidget(m_spinMsgPort, 0, 1);
    grid->addWidget(new QLabel("（客户端 TCP 60101）", gbListen), 0, 2);
    grid->addWidget(new QLabel("文件服务端口：", gbListen), 1, 0);
    grid->addWidget(m_spinFilePort, 1, 1);
    grid->addWidget(new QLabel("（客户端文件传输 60102）", gbListen), 1, 2);
    grid->addWidget(new QLabel("监听状态：", gbListen), 2, 0);
    grid->addWidget(m_labelState, 2, 1, 1, 2);
    grid->addWidget(new QLabel("在线用户：", gbListen), 3, 0);
    grid->addWidget(m_labelOnline, 3, 1);
    grid->addWidget(new QLabel("客户端连接：", gbListen), 4, 0);
    grid->addWidget(m_labelSocket, 4, 1);

    QHBoxLayout *btnRow = new QHBoxLayout;
    btnRow->addWidget(m_btnServerToggle);
    btnRow->addWidget(btnApply);
    btnRow->addStretch();
    grid->addLayout(btnRow, 5, 0, 1, 3);
    grid->setColumnStretch(2, 1);

    QGroupBox *gbEnv = new QGroupBox("运行环境", this);
    QFormLayout *form = new QFormLayout(gbEnv);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    form->addRow("数据库：", new QLabel(QDir::toNativeSeparators(
                                          MyApp::m_strDatabasePath + "server.db"), gbEnv));
    form->addRow("数据库大小：", m_labelDbSize = new QLabel("-", gbEnv));
    form->addRow("用户头像目录：", new QLabel(QDir::toNativeSeparators(MyApp::m_strHeadPath), gbEnv));
    form->addRow("文件接收目录：", new QLabel(QDir::toNativeSeparators(MyApp::m_strRecvPath), gbEnv));
    form->addRow("启动时间：", new QLabel(m_dtStart.toString("yyyy-MM-dd HH:mm:ss"), gbEnv));
    form->addRow("已运行：", m_labelRuntime = new QLabel("00:00:00", gbEnv));

    ui->verticalLayout_9->addWidget(gbListen);
    ui->verticalLayout_9->addWidget(gbEnv);
    ui->verticalLayout_9->addStretch();

    connect(btnApply, &QPushButton::clicked, this, &MainWindow::sltApplyServerConfig);
    connect(m_btnServerToggle, &QPushButton::clicked, this, &MainWindow::sltToggleServer);
}

void MainWindow::sltApplyServerConfig()
{
    const int newMsgPort = m_spinMsgPort->value();
    const int newFilePort = m_spinFilePort->value();

    if (newMsgPort == newFilePort) {
        showMsg(this, QMessageBox::Warning, "端口冲突", "消息端口和文件端口不能相同。");
        return;
    }
    if (newMsgPort == m_nMsgPort && newFilePort == m_nFilePort) {
        showMsg(this, QMessageBox::Information, "服务配置", "端口没有变化，无需应用。");
        return;
    }
    if (!askYesNo(this, "应用端口配置",
                  QString("将消息端口改为 %1、文件端口改为 %2。\n\n"
                          "保存后服务器会重新监听，客户端需要使用新端口重新连接。\n确定继续吗？")
                  .arg(newMsgPort).arg(newFilePort),
                  "应用", "取消")) {
        return;
    }

    // 消息服务：先关闭再按新端口监听
    m_tcpServer->CloseListen();
    if (!m_tcpServer->StartListen(newMsgPort)) {
        m_tcpServer->StartListen(m_nMsgPort);
        m_spinMsgPort->setValue(m_nMsgPort);
        showMsg(this, QMessageBox::Critical, "应用失败",
                QString("消息端口 %1 监听失败，可能已被占用。\n已恢复原端口 %2。")
                .arg(newMsgPort).arg(m_nMsgPort));
        appendLog(QString("消息端口 %1 应用失败，已恢复 %2").arg(newMsgPort).arg(m_nMsgPort),
                  QStringLiteral("#c62828"));
        return;
    }
    m_nMsgPort = newMsgPort;

    // 文件服务
    if (m_tcpFileServer) {
        m_tcpFileServer->stop();
        if (m_tcpFileServer->StartListen(static_cast<quint16>(newFilePort))) {
            m_nFilePort = newFilePort;
        } else {
            m_tcpFileServer->StartListen(static_cast<quint16>(m_nFilePort));
            m_spinFilePort->setValue(m_nFilePort);
            appendLog(QString("文件端口 %1 应用失败，已恢复 %2").arg(newFilePort).arg(m_nFilePort),
                      QStringLiteral("#c62828"));
            showMsg(this, QMessageBox::Warning, "文件端口",
                    QString("文件端口 %1 监听失败，已恢复原端口 %2。").arg(newFilePort).arg(m_nFilePort));
        }
    }

    saveConfig();
    refreshRuntimeInfo();
    updateTrayTip();
    notifyTray("服务配置已更新",
               QString("消息端口 %1 · 文件端口 %2").arg(m_nMsgPort).arg(m_nFilePort));
}

void MainWindow::sltToggleServer()
{
    if (!m_tcpServer) return;

    if (m_tcpServer->isListening()) {
        if (!askYesNo(this, "停止服务",
                      "停止监听后新的客户端将无法连接（已连接的客户端不受影响）。\n确定停止吗？",
                      "停止", "取消")) {
            return;
        }
        m_tcpServer->CloseListen();
        appendLog("消息服务已停止监听", QStringLiteral("#c62828"));
        notifyTray("服务已停止", "消息服务停止监听", true, QStringLiteral("#c62828"));
    } else {
        if (!m_tcpServer->StartListen(m_nMsgPort)) {
            showMsg(this, QMessageBox::Critical, "启动失败",
                    QString("端口 %1 监听失败，可能已被其它程序占用。").arg(m_nMsgPort));
            appendLog(QString("消息服务启动失败：端口 %1").arg(m_nMsgPort), QStringLiteral("#c62828"));
            return;
        }
        appendLog("消息服务已开始监听", QStringLiteral("#1a7f37"));
        notifyTray("服务已启动", QString("消息端口 %1 监听中").arg(m_nMsgPort));
    }
    refreshRuntimeInfo();
    updateTrayTip();
}

// ============================ 密码修改 ============================

void MainWindow::initPasswordPage()
{
    // 页面本身没有布局，groupBox 是绝对定位的，这里让它水平居中
    QLabel *tip = new QLabel("新密码至少 6 位，修改后下次登录请使用新密码", ui->groupBox_6);
    tip->setStyleSheet("color:#888888;font-size:12px;");
    tip->setAlignment(Qt::AlignCenter);
    ui->verticalLayout_2->insertWidget(1, tip);

    QVBoxLayout *pageLayout = new QVBoxLayout(ui->page_5);
    pageLayout->setContentsMargins(20, 20, 20, 20);
    pageLayout->addStretch();
    pageLayout->addWidget(ui->groupBox_6, 0, Qt::AlignHCenter);
    pageLayout->addStretch();
}

// ============================ 消息审计（端到端加密）============================

void MainWindow::initAuditPage()
{
    m_auditPage = new QWidget;
    m_auditPage->setObjectName("auditPage");

    QVBoxLayout *root = new QVBoxLayout(m_auditPage);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(8);

    // ---- 工具条 ----
    QHBoxLayout *bar = new QHBoxLayout;
    QLabel *title = new QLabel("消息审计");
    title->setStyleSheet("font-size:15px;font-weight:bold;color:#2C3E50;");
    bar->addWidget(title);

    m_labelMsgInfo = new QLabel("服务器只保存密文");
    m_labelMsgInfo->setStyleSheet("color:#8A96A3;font-size:12px;");
    bar->addSpacing(10);
    bar->addWidget(m_labelMsgInfo);
    bar->addStretch(1);

    m_btnMsgRefresh = new QPushButton("刷新");
    m_btnMsgRefresh->setObjectName("btnUserSearch");
    m_btnMsgRefresh->setCursor(Qt::PointingHandCursor);
    bar->addWidget(m_btnMsgRefresh);
    root->addLayout(bar);

    // ---- 说明 ----
    QLabel *tip = new QLabel("客户端之间用 AES-256-GCM 加密正文，AES 密钥再用各自 RSA 公钥包裹。"
                             "服务器和数据库里只有密文，且不持有任何私钥——即使拿到整台服务器，"
                             "也无法还原任何一条消息的内容。这里能看到的是加密后的报文与收发元数据。");
    tip->setWordWrap(true);
    tip->setStyleSheet("color:#8A96A3;font-size:12px;background:#F7FAFD;border:1px solid #E4EBF3;"
                       "border-radius:4px;padding:6px 8px;");
    root->addWidget(tip);

    // ---- 消息表 ----
    m_msgTable = new QTableWidget(0, 6, m_auditPage);
    m_msgTable->setHorizontalHeaderLabels({"时间", "发送者", "接收者", "类型", "内容", "状态"});
    m_msgTable->verticalHeader()->setVisible(false);
    m_msgTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_msgTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_msgTable->setAlternatingRowColors(true);
    m_msgTable->setShowGrid(false);
    m_msgTable->setStyleSheet(
        "QTableWidget{border:1px solid #D5DCE5;background:#FFFFFF;alternate-background-color:#FAFCFE;}"
        "QTableWidget::item{padding:4px 6px;}"
        "QTableWidget::item:selected{background:#DCEAFB;color:#1F2D3D;}"
        "QHeaderView::section{background:#F2F6FA;color:#5A6A7A;border:none;"
        "border-bottom:1px solid #E1E7EE;padding:6px 6px;font-weight:bold;}");

    QHeaderView *hh = m_msgTable->horizontalHeader();
    hh->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(4, QHeaderView::Stretch);
    hh->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    root->addWidget(m_msgTable, 1);

    connect(m_btnMsgRefresh, &QPushButton::clicked, this, &MainWindow::refreshAuditTable);

    // ---- 挂到页面栈 + 左侧导航 ----
    ui->stackedWidgetFunc->addWidget(m_auditPage);
    m_nAuditPageIndex = ui->stackedWidgetFunc->count() - 1;

    m_btnAuditPage = new QPushButton(QIcon(":/resource/images/ic_key.png"), "消息审计");
    m_btnAuditPage->setObjectName("btnAuditPage");
    m_btnAuditPage->setCheckable(true);
    m_btnAuditPage->setFocusPolicy(Qt::NoFocus);
    m_btnAuditPage->setCursor(Qt::PointingHandCursor);
    m_btnAuditPage->setIconSize(QSize(24, 24));
    const int pos = ui->verticalLayout_3->indexOf(ui->btnDataBackup);
    if (pos >= 0) {
        ui->verticalLayout_3->insertWidget(pos + 1, m_btnAuditPage);
    } else {
        ui->verticalLayout_3->addWidget(m_btnAuditPage);
    }
}

void MainWindow::refreshAuditTable()
{
    if (!m_msgTable || !m_labelMsgInfo) return;

    m_msgTable->setRowCount(0);
    if (!m_bLoggedIn) {
        m_labelMsgInfo->setText("请先登录服务器管理账号");
        return;
    }

    DataBaseMag *db = DataBaseMag::getInstance();
    const QList<QVariantMap> rows = db->getMsgLog(300);

    int encCount = 0, plainCount = 0;

    m_msgTable->setUpdatesEnabled(false);
    for (int i = 0; i < rows.size(); ++i) {
        const QVariantMap &row = rows.at(i);
        const QJsonObject obj = QJsonDocument::fromJson(row.value("payload").toString().toUtf8()).object();
        const QString rawMsg = obj.value("msg").toString();
        const bool encrypted = (obj.value("enc").toInt() != 0) && CryptoUtil::isPacket(rawMsg);

        QString content;
        QString state;
        if (encrypted) {
            ++encCount;
            content = rawMsg;
            // 服务器能看到的极限：这份密文是用哪几个人的公钥分别包裹过密钥的。
            // 至于正文，没有对应私钥就永远打不开——本程序里也不存在任何私钥副本。
            const QList<int> rcpt = CryptoUtil::packetRecipients(rawMsg);
            state = rcpt.isEmpty() ? QStringLiteral("端到端加密")
                                   : QStringLiteral("端到端加密·%1份密钥").arg(rcpt.size());
        } else {
            ++plainCount;
            content = rawMsg;
            state = QStringLiteral("未加密");
        }

        const int r = m_msgTable->rowCount();
        m_msgTable->insertRow(r);

        // 时间
        QTableWidgetItem *timeItem = new QTableWidgetItem(row.value("time").toString());
        timeItem->setToolTip(row.value("time").toString());
        m_msgTable->setItem(r, 0, timeItem);

        // 发送者
        const int fromId = row.value("fromId").toInt();
        QString fromName = row.value("fromName").toString();
        if (fromName.isEmpty()) fromName = QString("用户%1").arg(fromId);
        m_msgTable->setItem(r, 1, new QTableWidgetItem(QString("%1(%2)").arg(fromName).arg(fromId)));

        // 接收者
        const int toId = row.value("toId").toInt();
        QString toName = row.value("toName").toString();
        if (toName.isEmpty()) toName = QString("用户%1").arg(toId);
        if (row.value("isGroup").toBool()) toName = QString("群·%1(%2)").arg(toName).arg(toId);
        else toName = QString("%1(%2)").arg(toName).arg(toId);
        m_msgTable->setItem(r, 2, new QTableWidgetItem(toName));

        // 类型
        QString typeName;
        switch (row.value("type").toInt()) {
        case Text:    typeName = "文字"; break;
        case Audio:   typeName = "语音"; break;
        case Picture: typeName = "图片"; break;
        case Files:   typeName = "文件"; break;
        case Face:    typeName = "表情"; break;
        case SendMsg:
        case SendGroupMsg:  typeName = "文字"; break;   // 早期记录存的是协议码
        default:      typeName = "其他"; break;
        }
        m_msgTable->setItem(r, 3, new QTableWidgetItem(typeName));

        // 内容：密文不可读，只给出长度和算法；完整报文放 tooltip，便于演示"服务器只看到密文"
        QString preview;
        if (encrypted) {
            preview = QStringLiteral("[密文 %1 字节 · AES-256-GCM]").arg(rawMsg.size());
        } else {
            preview = content;
            preview.replace('\n', ' ');
            if (preview.size() > 80) preview = preview.left(80) + "…";
        }
        QTableWidgetItem *contentItem = new QTableWidgetItem(preview);
        contentItem->setToolTip(content.size() > 400 ? content.left(400) + "…" : content);
        if (encrypted) {
            contentItem->setForeground(QBrush(QColor("#8A96A3")));
        } else {
            contentItem->setForeground(QBrush(QColor("#B26A00")));
        }
        m_msgTable->setItem(r, 4, contentItem);

        // 状态
        QTableWidgetItem *stateItem = new QTableWidgetItem(state);
        if (encrypted) stateItem->setForeground(QBrush(QColor("#1A7F37")));
        else stateItem->setForeground(QBrush(QColor("#B26A00")));
        m_msgTable->setItem(r, 5, stateItem);
    }
    m_msgTable->setUpdatesEnabled(true);

    m_labelMsgInfo->setText(QString("共 %1 条 · 端到端加密 %2 条 · 未加密 %3 条 · 服务器无任何私钥，密文无法还原")
                            .arg(rows.size())
                            .arg(encCount)
                            .arg(plainCount));
}

void MainWindow::on_btnPasswdChange_clicked()
{
    if (!m_bLoggedIn || m_strLoginUser.isEmpty()) {
        showMsg(this, QMessageBox::Information, "修改密码", "请先登录管理员账号。");
        return;
    }

    const QString oldPwd = ui->lineEditOldPasswd->text();
    const QString newPwd = ui->lineEditNewPasswd->text();
    const QString newPwd2 = ui->lineEditNewPasswd2->text();

    if (oldPwd.isEmpty() || newPwd.isEmpty() || newPwd2.isEmpty()) {
        showMsg(this, QMessageBox::Warning, "修改密码", "请完整填写旧密码和两次新密码。");
        return;
    }
    if (newPwd != newPwd2) {
        showMsg(this, QMessageBox::Warning, "修改密码", "两次输入的新密码不一致。");
        return;
    }
    if (newPwd.length() < 6) {
        showMsg(this, QMessageBox::Warning, "修改密码", "新密码长度不能少于 6 位。");
        return;
    }
    if (newPwd == oldPwd) {
        showMsg(this, QMessageBox::Warning, "修改密码", "新密码不能与旧密码相同。");
        return;
    }
    if (!DataBaseMag::getInstance()->checkPassword(m_strLoginUser, oldPwd)) {
        showMsg(this, QMessageBox::Warning, "修改密码", "旧密码不正确。");
        return;
    }
    if (!DataBaseMag::getInstance()->updatePassword(m_strLoginUser, newPwd)) {
        showMsg(this, QMessageBox::Critical, "修改密码", "密码写入数据库失败，请查看日志。");
        return;
    }

    ui->lineEditOldPasswd->clear();
    ui->lineEditNewPasswd->clear();
    ui->lineEditNewPasswd2->clear();

    appendLog(QString("账号 %1 的密码已修改").arg(m_strLoginUser), QStringLiteral("#1a7f37"));
    notifyTray("密码修改成功", QString("账号 %1 的新密码已生效，下次登录请使用新密码").arg(m_strLoginUser));
    showMsg(this, QMessageBox::Information, "修改密码", "密码修改成功。");
}

// ============================ 数据备份 ============================

void MainWindow::initBackupPage()
{
    ui->groupBox->setTitle("数据备份");
    ui->lineEditBackup->setText(QDir::toNativeSeparators(m_strBackupDir));
    connect(ui->toolButton, &QToolButton::clicked, this, &MainWindow::sltChooseBackupDir);

    m_backupList = new QListWidget(this);
    m_backupList->setAlternatingRowColors(true);
    m_backupList->setSelectionMode(QAbstractItemView::SingleSelection);

    QHBoxLayout *row = new QHBoxLayout;
    QLabel *tip = new QLabel("已有备份文件（选中后点「还原」）");
    QPushButton *btnRefresh = new QPushButton("刷新列表");
    row->addWidget(tip);
    row->addStretch();
    row->addWidget(btnRefresh);

    // verticalLayout_10: 0=路径行, 1=spacer，插在两者之间
    ui->verticalLayout_10->insertLayout(1, row);
    ui->verticalLayout_10->insertWidget(2, m_backupList);

    // 让备份列表占满剩余空间，而不是被下面的 spacer 压成一条
    if (ui->verticalSpacer_2) {
        ui->verticalSpacer_2->changeSize(20, 0, QSizePolicy::Minimum, QSizePolicy::Minimum);
    }
    ui->verticalLayout_10->setStretch(2, 1);

    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::refreshBackupList);
}

void MainWindow::sltChooseBackupDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
                this, "选择备份目录", m_strBackupDir, QFileDialog::ShowDirsOnly);
    if (dir.isEmpty()) return;

    m_strBackupDir = QDir(dir).absolutePath();
    ui->lineEditBackup->setText(QDir::toNativeSeparators(m_strBackupDir));
    saveConfig();
    refreshBackupList();
    appendLog(QString("备份目录已设置为：%1").arg(QDir::toNativeSeparators(m_strBackupDir)));
}

void MainWindow::refreshBackupList()
{
    if (!m_backupList) return;
    m_backupList->clear();

    QDir dir(m_strBackupDir);
    if (!dir.exists()) return;

    const QFileInfoList files = dir.entryInfoList(QStringList() << "*.db",
                                                  QDir::Files, QDir::Time);
    for (const QFileInfo &info : files) {
        QListWidgetItem *item = new QListWidgetItem(
                    QString("%1    %2    %3")
                    .arg(info.fileName(),
                         sizeText(info.size()),
                         info.lastModified().toString("yyyy-MM-dd HH:mm:ss")));
        item->setData(Qt::UserRole, info.absoluteFilePath());
        item->setToolTip(info.absoluteFilePath());
        m_backupList->addItem(item);
    }

    if (files.isEmpty()) {
        QListWidgetItem *item = new QListWidgetItem("目录中还没有备份文件");
        item->setForeground(QColor("#999999"));
        item->setFlags(Qt::NoItemFlags);
        m_backupList->addItem(item);
    }
}

void MainWindow::on_btnBackup_clicked()
{
    if (m_strBackupDir.isEmpty()) {
        sltChooseBackupDir();
        if (m_strBackupDir.isEmpty()) return;
    }

    QDir().mkpath(m_strBackupDir);
    const QString file = QString("%1/server_%2.db")
            .arg(m_strBackupDir,
                 QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));

    QString err;
    if (!backupDatabase(file, &err)) {
        showMsg(this, QMessageBox::Critical, "备份失败", err);
        return;
    }

    refreshBackupList();
    const QFileInfo info(file);
    notifyTray("数据备份成功",
               QString("%1（%2）").arg(info.fileName(), sizeText(info.size())));
    showMsg(this, QMessageBox::Information, "备份完成",
            QString("数据库已备份到：\n%1").arg(QDir::toNativeSeparators(file)));
}

bool MainWindow::backupDatabase(const QString &filePath, QString *err)
{
    auto fail = [err](const QString &msg) {
        if (err) *err = msg;
        return false;
    };

    if (filePath.isEmpty()) return fail("备份路径为空");
    QDir().mkpath(QFileInfo(filePath).absolutePath());

    const QString dbPath = MyApp::m_strDatabasePath + "server.db";
    if (QFileInfo(filePath).absoluteFilePath() == QFileInfo(dbPath).absoluteFilePath()) {
        return fail("备份文件不能与正在使用的数据库是同一个文件");
    }
    if (!QFile::exists(dbPath)) return fail("找不到服务器数据库文件");
    if (QFile::exists(filePath) && !QFile::remove(filePath)) {
        return fail("目标文件已存在且无法覆盖，请换个文件名");
    }

    // 优先用 SQLite 的在线备份（VACUUM INTO），不需要中断服务
    QSqlQuery query(QSqlDatabase::database());
    QString escaped = filePath;
    escaped.replace("'", "''");
    if (query.exec(QString("VACUUM INTO '%1';").arg(escaped))) {
        return true;
    }
    qDebug() << "[备份] VACUUM INTO 不可用，改用文件复制：" << query.lastError().text();
    query.finish();

    // 回退方案：关闭连接后复制文件
    DataBaseMag::getInstance()->closeDatabase();
    const bool copied = QFile::copy(dbPath, filePath);
    DataBaseMag::getInstance()->openDatabase(dbPath);
    if (!copied) {
        return fail("复制数据库文件失败，请检查目标目录权限");
    }
    return true;
}

void MainWindow::on_btnDataUdo_clicked()
{
    QString src;
    if (m_backupList && m_backupList->currentItem()) {
        src = m_backupList->currentItem()->data(Qt::UserRole).toString();
    }
    if (src.isEmpty()) {
        src = QFileDialog::getOpenFileName(this, "选择要还原的备份文件", m_strBackupDir,
                                           "SQLite 数据库 (*.db);;所有文件 (*.*)");
    }
    if (src.isEmpty()) return;
    if (!QFile::exists(src)) {
        showMsg(this, QMessageBox::Warning, "还原数据库", "备份文件不存在。");
        return;
    }

    if (!askYesNo(this, "还原数据库",
                  QString("将使用以下备份覆盖当前数据库：\n%1\n\n"
                          "还原前会自动把当前数据备份到 Data/Backup 目录。\n"
                          "还原完成后建议重启服务端程序。确定继续吗？")
                  .arg(QDir::toNativeSeparators(src)),
                  "还原", "取消")) {
        return;
    }

    QString err;
    if (!restoreDatabase(src, &err)) {
        showMsg(this, QMessageBox::Critical, "还原失败", err);
        return;
    }

    refreshUserTable();
    clearUserForm();
    refreshBackupList();
    if (m_lineSearch) m_lineSearch->clear();
    sltSearchUser();

    appendLog(QString("数据库已从备份还原：%1").arg(QFileInfo(src).fileName()),
              QStringLiteral("#c62828"));
    notifyTray("还原完成", "数据库已还原，建议重启服务端程序",
               true, QStringLiteral("#c62828"));
    showMsg(this, QMessageBox::Information, "还原完成",
            "数据库已还原。为保证客户端连接状态正确，建议重启服务端程序。");
}

bool MainWindow::restoreDatabase(const QString &filePath, QString *err)
{
    auto fail = [err](const QString &msg) {
        if (err) *err = msg;
        return false;
    };

    const QString dbPath = MyApp::m_strDatabasePath + "server.db";
    if (QFileInfo(filePath).absoluteFilePath() == QFileInfo(dbPath).absoluteFilePath()) {
        return fail("不能用当前正在使用的数据库文件还原");
    }

    // 校验是不是一个 SQLite 数据库
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return fail("备份文件无法读取");
    const QByteArray magic = file.read(16);
    file.close();
    if (!magic.startsWith("SQLite format 3")) {
        return fail("该文件不是有效的 SQLite 数据库备份");
    }

    // 还原前先把当前数据库整体备份一份
    const QString safeFile = QString("%1before_restore_%2.db")
            .arg(MyApp::m_strBackupPath,
                 QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    QString backupErr;
    if (!backupDatabase(safeFile, &backupErr)) {
        return fail(QString("还原前备份当前数据库失败：%1").arg(backupErr));
    }

    DataBaseMag::getInstance()->closeDatabase();
    QFile::remove(dbPath);
    const bool copied = QFile::copy(filePath, dbPath);

    if (!copied || !DataBaseMag::getInstance()->openDatabase(dbPath)) {
        // 失败了就回滚到还原前的备份
        QFile::remove(dbPath);
        QFile::copy(safeFile, dbPath);
        DataBaseMag::getInstance()->openDatabase(dbPath);
        return fail("还原失败，已自动恢复为还原前的数据");
    }

    // 还原后群成员缓存要重新加载，否则群消息会按旧成员转发
    if (m_tcpServer) {
        m_tcpServer->reloadGroupCache();
    }
    appendLog(QString("还原前的数据库已保存为：%1")
              .arg(QDir::toNativeSeparators(safeFile)));
    return true;
}

// ============================ 配置读写 ============================

void MainWindow::loadConfig()
{
    m_nMsgPort = MyApp::GetSettingKeyValue("ServerCfg", "MsgPort", 60101).toInt();
    m_nFilePort = MyApp::GetSettingKeyValue("ServerCfg", "FilePort", 60102).toInt();
    m_strBackupDir = MyApp::GetSettingKeyValue("ServerCfg", "BackupDir",
                                               MyApp::m_strBackupPath).toString();

    if (m_nMsgPort < 1024 || m_nMsgPort > 65535) m_nMsgPort = 60101;
    if (m_nFilePort < 1024 || m_nFilePort > 65535) m_nFilePort = 60102;
    if (m_strBackupDir.isEmpty()) m_strBackupDir = MyApp::m_strBackupPath;
    if (m_nMsgPort == m_nFilePort) m_nFilePort = m_nMsgPort + 1;
}

void MainWindow::saveConfig()
{
    MyApp::SetSettingFile("ServerCfg", "MsgPort", m_nMsgPort);
    MyApp::SetSettingFile("ServerCfg", "FilePort", m_nFilePort);
    MyApp::SetSettingFile("ServerCfg", "BackupDir", m_strBackupDir);
}

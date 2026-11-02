#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QButtonGroup>
#include <QFileInfo>
#include <QMenu>
#include <QAction>
#include <qqcell.h>
#include "comapi/myapp.h"
#include <QInputDialog>
#include <QJsonObject>
#include <QProcess>
#include <QDir>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QEvent>
#include <QFileDialog>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QUuid>
#include <QFile>
#include <QImage>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QCalendarWidget>
#include <QDesktopServices>
#include <QUrl>
#include "netdb/databasemsg.h"
#include <netdb/databasemsg.h>
#include "netdb/keystore.h"
#include "comapi/cryptoutil.h"
#include "comapi/unit.h"
#include <basewidget/iteminfo.h>
#include <comapi/global.h>
#include <uipage/chatwindow.h>
#include <uipage/groupmemberdialog.h>

MainWindow::MainWindow(MySocket *socket, QWidget *parent) :
    CustomMoveWidget(parent),
    ui(new Ui::MainWindow)
{
    m_tcpSocket = socket;
    m_tcpSocket->setParent(this);
    ui->setupUi(this);
    setQss(QString(":/qss/resource/qss/default.css"));
    // 无边框窗口默认没有 WS_MINIMIZEBOX，Windows 会认为不可最小化，
    // 点任务栏图标毫无反应；显式声明"可最小化 + 系统菜单"即可（界面依旧无边框）
    setWindowFlags(Qt::FramelessWindowHint
                   | Qt::WindowMinimizeButtonHint
                   | Qt::WindowSystemMenuHint);
    ui->GCStackedWidget->setCurrentIndex(0);

    m_buttonGroup = new QButtonGroup(this);
    m_buttonGroup->addButton(ui->btnFrind,0);
    m_buttonGroup->addButton(ui->btnGroup,1);
    m_buttonGroup->addButton(ui->btnConversation,2);
    m_buttonGroup->addButton(ui->btnApplay,3);

    connect(m_buttonGroup,SIGNAL(buttonClicked(int)),this,SLOT(sltButtonClicked(int)));
    connect(m_tcpSocket,&MySocket::signalStatus,this,&MainWindow::sltStatus);
    connect(m_tcpSocket,&MySocket::signalConnectSuccess,this,[=](){
        qDebug() << "MainWindow连接服务器成功";
        updateTrayTip();
    });

    // ===== 断线自动重连 =====
    // MySocket 负责 TCP 层按退避重试，这里负责"连上以后把登录状态恢复回来"
    connect(m_tcpSocket, &MySocket::signalConnectionLost,
            this, [this](int attempt, int delayMs) {
        const int seconds = qMax(1, delayMs / 1000);
        setConnState(false, tr("%1 秒后第 %2 次重连").arg(seconds).arg(attempt));
        notifyTray(tr("连接已断开"),
                   tr("与服务器的连接中断，%1 秒后自动重连（第 %2 次）")
                   .arg(seconds).arg(attempt));

        // 断线瞬间就把"还在等回执"的消息标红，用户立刻知道没发出去
        failAllPendingSends(tr("连接已断开"));
    });
    connect(m_tcpSocket, &MySocket::signalReconnected,
            this, &MainWindow::sltServerReconnected);

    m_reloginTimer = new QTimer(this);
    m_reloginTimer->setSingleShot(true);
    m_reloginTimer->setInterval(8000);
    connect(m_reloginTimer, &QTimer::timeout, this, &MainWindow::sltReloginTimeout);

    // 端到端加密：对方公钥可能晚一步才上传，定时重试补发排队的消息
    m_sendRetryTimer = new QTimer(this);
    m_sendRetryTimer->setInterval(5000);
    connect(m_sendRetryTimer, &QTimer::timeout, this, &MainWindow::sltRetryPendingMessages);
    m_sendRetryTimer->start();

    // 本机密钥必须在收到任何消息之前就绪：它是本地生成的，不需要等服务器。
    // 以前挂在"收到管理员公钥"上，导致登录瞬间下发的离线消息全部解不开。
    initLocalCrypto();

    // 心跳：让服务器知道本端还活着，异常退出（被杀进程/断网）时能尽快判离线
    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(15000);
    connect(m_heartbeatTimer, &QTimer::timeout, this, [this]() {
        if (!m_tcpSocket || !m_tcpSocket->isConnected()) return;
        QJsonObject beat;
        beat.insert("t", QDateTime::currentMSecsSinceEpoch());
        m_tcpSocket->sendMessage(HeartBeat, QJsonValue(beat));
    });
    m_heartbeatTimer->start();

    m_bQuit = false;

    // 天气小程序的进程对象，退出主程序时一并结束它
    m_weatherProcess = new QProcess(this);
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::stopWeather);

    InitSysMenu();
    InitSysTrayIcon();
    InitQQListMenu();
    initAppPage();

    // 应用页：双击某个应用打开它
    connect(ui->widget_5, SIGNAL(onChildDidDoubleClicked(QQCell*)),
            this, SLOT(SltAppCellClicked(QQCell*)));

    // 会话页：和好友/群组页一样的列表，双击打开对应聊天窗口
    connect(ui->convationListWidget, SIGNAL(onChildDidDoubleClicked(QQCell*)),
            this, SLOT(SltConversationClicked(QQCell*)));

    // 登录后先把上次没读的消息统计出来（离线期间的未读保存在消息库里）
    refreshConversationList();
    refreshUnreadBadges();

    // 会话页右键：删除会话（只删聊天记录，不删好友）
    QMenu *convMenu = new QMenu(this);
    convMenu->addAction(tr("删除会话"));
    ui->convationListWidget->setChildPopMenu(convMenu);
    connect(convMenu, &QMenu::triggered, this, &MainWindow::onConvMenuSelected);

    // 申请列表页（好友申请 / 加群申请），挂在堆叠页最后
    m_requestPage = new QWidget(ui->GCStackedWidget);
    m_requestPage->setObjectName("requestPage");
    QVBoxLayout *reqLayout = new QVBoxLayout(m_requestPage);
    reqLayout->setContentsMargins(0, 0, 0, 0);
    reqLayout->setSpacing(0);
    QLabel *reqTitle = new QLabel(tr("申请列表"), m_requestPage);
    reqTitle->setObjectName("requestPageTitle");
    reqTitle->setAlignment(Qt::AlignCenter);
    reqTitle->setFixedHeight(30);
    reqTitle->setStyleSheet("QLabel#requestPageTitle{background:#387ECC;color:#FFFFFF;font:bold 13px '微软雅黑';}");
    reqLayout->addWidget(reqTitle);
    m_requestList = new QListWidget(m_requestPage);
    m_requestList->setObjectName("requestListWidget");
    m_requestList->setStyleSheet("QListWidget#requestListWidget{background:#FFFFFF;border:none;}");
    reqLayout->addWidget(m_requestList, 1);
    ui->GCStackedWidget->addWidget(m_requestPage);

    rebuildRequestPage();

    // 自己的头像：右键可以换；本地缺文件（比如换了台机器登录）就先向服务器要
    ui->widgetHead->installEventFilter(this);
    ui->widgetHead->setCursor(Qt::PointingHandCursor);
    ui->widgetHead->setToolTip(tr("右键更换头像"));
    MyApp::setDefaultHead();
    if (MyApp::m_strHeadFile.isEmpty()) {
        MyApp::m_strHeadFile = QStringLiteral("default.png");
    }
    // 本地还没有自己的头像文件时先显示默认头像，等服务器下发后再换过来
    const QString ownHeadPath = MyApp::HeadFullPath(MyApp::m_strHeadFile);
    setHead(QFile::exists(ownHeadPath) ? ownHeadPath : MyApp::HeadFullPath(QStringLiteral("default.png")));
    ensureHeadAvailable(MyApp::m_nId, MyApp::m_strHeadFile);
    ui->labelUser->setText(MyApp::m_strUserName);


}

MainWindow::~MainWindow()
{
    stopWeather();

    delete ui;
    ui = nullptr;
}

void MainWindow::sltButtonClicked(int index)
{
    ui->GCStackedWidget->setCurrentIndex(index);

    // 切换页面时刷新一次，保证看到的一定是最新的会话和未读
    if (2 == index) {
        refreshConversationList();
    }
    refreshUnreadBadges();
}

void MainWindow::InitSysMenu()
{
    // 设置子菜单
    QMenu *sysmenu = new QMenu(this);
    sysmenu->addAction(tr("系统设置"));
    sysmenu->addAction("消息管理器");
    sysmenu->addAction("文件助手");
    sysmenu->addSeparator();
    sysmenu->addAction("修改密码");
    sysmenu->addAction("帮助");
    sysmenu->addAction("连接服务器");
    sysmenu->addSeparator();
    sysmenu->addAction("升级");

    // 添加菜单
    ui->btnSysMenu->setMenu(sysmenu);
    connect(sysmenu, SIGNAL(triggered(QAction*)), this, SLOT(SltSysmenuCliecked(QAction*)));
}


void MainWindow::InitSysTrayIcon()
{
    systemTrayIcon = new QSystemTrayIcon(this);
    systemTrayIcon->setIcon(QIcon(":/resource/background/app.png"));

    QMenu *trayMenu = new QMenu(this);
    QAction *actShow = trayMenu->addAction(tr("显示主面板"));
    actShow->setData(0);
    trayMenu->addSeparator();
    QAction *actOnline = trayMenu->addAction(tr("我在线上"));
    actOnline->setData(1);
    QAction *actOffline = trayMenu->addAction(tr("离线"));
    actOffline->setData(2);
    trayMenu->addSeparator();
    QAction *actQuit = trayMenu->addAction(tr("退出"));
    actQuit->setData(3);

    systemTrayIcon->setContextMenu(trayMenu);
    systemTrayIcon->show();
    updateTrayTip();

    connect(systemTrayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)),this, SLOT(SltTrayIcoClicked(QSystemTrayIcon::ActivationReason)));
    connect(trayMenu, SIGNAL(triggered(QAction*)), this, SLOT(SltTrayIconMenuClicked(QAction*)));
}

/**
 * @brief MainWindow::SltTrayIcoClicked
 * 托盘图标：单击切换显示/收起，双击显示（Windows 上双击会先来一次单击，这里去重）
 */
void MainWindow::SltTrayIcoClicked(QSystemTrayIcon::ActivationReason reason)
{
    if (m_bQuit) {
        return;
    }

    switch (reason) {
    case QSystemTrayIcon::Trigger: {
        m_msLastTrayClick = QDateTime::currentMSecsSinceEpoch();
        // 纯切换：窗口显示着就收起，收起/最小化了就显示
        if (isWindowShowing()) {
            hideToTray();
        } else {
            showNormal();
            raise();
            activateWindow();
        }
        break;
    }
    case QSystemTrayIcon::DoubleClick:
        if (QDateTime::currentMSecsSinceEpoch() - m_msLastTrayClick > 500) {
            showNormal();
            raise();
            activateWindow();
        }
        break;
    case QSystemTrayIcon::MiddleClick:
        showNormal();
        raise();
        activateWindow();
        break;
    default:
        break;
    }
}
/**
 * @brief MainWindow::InitQQListMenu
 * 添加好友列表菜单
 */
void MainWindow::InitQQListMenu()
{
    //默认项
    QQCell *myFriend = new QQCell;
    myFriend->groupName = QString(tr("我的好友"));
    myFriend->isOpen = false;
    myFriend->type = QQCellType_Group;
    myFriend->name = QString(tr("我的好友"));
    myFriend->subTitle = QString("(0/0)");
    ui->frindListWidget->insertQQCell(myFriend);

    QQCell *blacklist = new QQCell;
    blacklist->groupName = QString(tr("黑名单"));
    blacklist->isOpen = false;
    blacklist->type = QQCellType_Group;
    blacklist->name = QString(tr("黑名单"));
    blacklist->subTitle = QString("(0/0)");
    ui->frindListWidget->insertQQCell(blacklist);

    connect(ui->frindListWidget, SIGNAL(onChildDidDoubleClicked(QQCell*)), this, SLOT(SltFriendsClicked(QQCell*)));

    //菜单
    QMenu *addFriend = new QMenu(this);
    addFriend->addAction(tr("添加好友"));
    addFriend->addAction(tr("刷新"));
    addFriend->addSeparator();
    addFriend->addAction(tr("删除该组"));
    ui->frindListWidget->setGroupPopMenu(addFriend);
    connect(addFriend,SIGNAL(triggered(QAction*)),this,SLOT(onAddFriendMenuDidSelected(QAction*)));

    //子菜单
    QMenu *childMenu = new QMenu(this);
    childMenu->addAction(tr("发送即时消息"));
    childMenu->addSeparator();
    childMenu->addAction("移动到黑名单");
    childMenu->addAction("删除联系人");
    //***********疑惑************
    QMenu *groupListMenu = new QMenu(tr("移动联系人至"));
    childMenu->addMenu(groupListMenu);
    // childMenu->addAction(tr("创建讨论组"));
    connect(childMenu, SIGNAL(triggered(QAction*)), this, SLOT(onChildPopMenuDidSelected(QAction*)));
    ui->frindListWidget->setChildPopMenu(childMenu);

    //组列表
    QMenu *myGroupMenu = new QMenu(this);
    myGroupMenu->addAction(tr("创建讨论组"));
    myGroupMenu->addAction(tr("加入讨论组"));
    myGroupMenu->addAction(tr("删除该组"));
    myGroupMenu->addSeparator();
    myGroupMenu->addAction(tr("刷新"));
    myGroupMenu->addSeparator();
    connect(myGroupMenu, SIGNAL(triggered(QAction*)), this, SLOT(onGroupPopMenuDidSelected(QAction*)));
    ui->groupListWidget->setGroupPopMenu(myGroupMenu);
    //设置子菜单
    QMenu *groupChildMen = new QMenu(this);
    groupChildMen->addAction(tr("发送即时消息"));
    groupChildMen->addAction(tr("群成员"));
    groupChildMen->addSeparator();
    groupChildMen->addAction(tr("退出群组"));
    groupChildMen->addAction(tr("解散群聊"));
    ui->groupListWidget->setChildPopMenu(groupChildMen);
    connect(groupChildMen, &QMenu::triggered, this, &MainWindow::onGroupMenuDidSelected);

    // 菜单弹出前按"群主/成员"身份显示对应的操作
    connect(ui->groupListWidget, SIGNAL(on_popmenu_will_show(QQListWidget*,QQPopMenuWidget*,QMenu*)),
            this, SLOT(SltGroupMenuWillShow(QQListWidget*,QQPopMenuWidget*,QMenu*)));

    //添加默认项
    QQCell *groupCell = new QQCell;
    groupCell->groupName = QString(tr("我的群组"));
    groupCell->isOpen = false;
    groupCell->type = QQCellType_GroupEx;
    groupCell->name = QString(tr("讨论组"));
    groupCell->subTitle = QString("(0/0)");
    ui->groupListWidget->insertQQCell(groupCell);

    connect(ui->groupListWidget, SIGNAL(onChildDidDoubleClicked(QQCell*)), this, SLOT(SltGroupsClicked(QQCell*)));

}
// 托盘菜单
void MainWindow::SltTrayIconMenuClicked(QAction *action)
{
    if (nullptr == action) {
        return;
    }

    switch (action->data().toInt()) {
    case 0:     // 显示主面板
        showNormal();
        raise();
        activateWindow();
        break;
    case 1:     // 我在线上
        if (m_tcpSocket->state() == QAbstractSocket::ConnectedState) {
            m_tcpSocket->CheckConnected();
            notifyTray(tr("状态"), tr("当前已在线"));
        } else {
            reconnectServer();
        }
        break;
    case 2:     // 离线
        if (CMessageBox::Question(this,
                                  tr("确定要断开与服务器的连接吗？\n断开后将收不到新消息。"),
                                  tr("离线")) != 1) {
            break;
        }
        m_tcpSocket->ColseConnected();
        updateTrayTip();
        notifyTray(tr("已离线"), tr("已断开与服务器的连接，可在托盘菜单重新上线"));
        break;
    case 3:     // 退出
        m_bQuit = true;
        this->hide();
        if (systemTrayIcon) {
            systemTrayIcon->hide();
        }
        QTimer::singleShot(300, this, SLOT(SltQuitApp()));
        break;
    default:
        break;
    }
}

/**
 * @brief MainWindow::closeEvent
 * 关闭窗口 = 收起托盘，消息服务不停
 */
void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_bQuit || !QSystemTrayIcon::isSystemTrayAvailable() || nullptr == systemTrayIcon) {
        event->accept();
        return;
    }

    event->ignore();
    hideToTray();
}

bool MainWindow::isWindowShowing() const
{
    return isVisible() && !isMinimized();
}

void MainWindow::hideToTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable() || nullptr == systemTrayIcon) {
        showMinimized();
        return;
    }

    hide();

    if (!m_bTrayTipShown && systemTrayIcon->isVisible()) {
        systemTrayIcon->showMessage(tr("畅想聊天仍在后台运行"),
                                    tr("单击托盘图标可再次打开主面板，右键可退出。"),
                                    QSystemTrayIcon::Information, 4000);
        m_bTrayTipShown = true;
    }
}

void MainWindow::updateTrayTip()
{
    if (nullptr == systemTrayIcon) {
        return;
    }

    const bool online = m_tcpSocket
            && m_tcpSocket->state() == QAbstractSocket::ConnectedState;
    systemTrayIcon->setToolTip(QString("%1（%2）\n%3\n服务器：%4:%5")
                               .arg(MyApp::m_strUserName.isEmpty() ? tr("未登录") : MyApp::m_strUserName)
                               .arg(online ? tr("在线") : tr("离线"))
                               .arg(QString("畅想聊天APP v%1").arg(MY_VERSION_STR))
                               .arg(MyApp::m_strHostAddr)
                               .arg(MyApp::m_nMsgPort));
}
void MainWindow::SltQuitApp()
{
    //    // 关闭所有的聊天窗口
    //    foreach (ChatWindow *window, m_chatFriendWindows) {
    //        window->close();
    //    }

    //    foreach (ChatWindow *window, m_chatGroupWindows) {
    //        window->close();
    //    }

    // 关闭socket
    m_tcpSocket->deleteLater();

    // 天气小程序随主程序一起关闭
    stopWeather();

    delete ui;
    ui = nullptr;      // 析构函数里会再 delete 一次，这里置空避免重复释放
    qApp->quit();
}

///////////////////////////////////////////////////////////////////////////
/// 标题栏按钮 / 系统菜单 / 应用页
///

void MainWindow::on_btnWinMin_clicked()
{
    showMinimized();     // 最小化到任务栏，任务栏图标单击可还原
}

void MainWindow::on_btnWinClose_clicked()
{
    close();             // 走 closeEvent：收进托盘，聊天服务不停
}

void MainWindow::on_btnWinMenu_clicked()
{
    // 标题栏上的"设置"按钮：直接弹出系统菜单（与左下角系统菜单同一份）
    QMenu *menu = ui->btnSysMenu->menu();
    if (nullptr == menu) {
        return;
    }
    menu->exec(ui->btnWinMenu->mapToGlobal(QPoint(0, ui->btnWinMenu->height())));
}

void MainWindow::SltSysmenuCliecked(QAction *action)
{
    if (nullptr == action) {
        return;
    }

    const QString text = action->text();
    if (text == tr("系统设置")) {
        openServerConfig();
    } else if (text == tr("消息管理器")) {
        openMessageManager();
    } else if (text == tr("文件助手")) {
        openFileHelper();
    } else if (text == tr("修改密码")) {
        openChangePasswd();
    } else if (text == tr("帮助")) {
        openAbout();
    } else if (text == tr("连接服务器")) {
        reconnectServer();
    } else if (text == tr("升级")) {
        checkUpgrade();
    }
}

/**
 * @brief MainWindow::initAppPage
 * 应用页（第四个标签）：常用功能入口，双击打开
 */
void MainWindow::initAppPage()
{
    ui->widget_5->clearAllCells();

    QQCell *group = new QQCell;
    group->groupName = tr("应用中心");
    group->name = tr("应用中心");
    group->subTitle = QString();
    group->type = QQCellType_GroupEx;   // 只显示组名，不显示"在线人数"
    group->isOpen = true;
    ui->widget_5->insertQQCell(group);

    struct AppEntry { int id; QString name; QString desc; QString icon; };
    QVector<AppEntry> apps;
    // 注意：resource/common 下多半是给绿色标题栏用的白色图标，白底列表里会看不见，
    // 这里统一挑深色的那几张
    apps << AppEntry{1, tr("天气查询"), tr("打开本地天气小程序"), ":/resource/common/ic_weather.png"}
         << AppEntry{2, tr("日历"),      tr("查看日期与安排"),       ":/resource/common/ic_calendar.png"}
         << AppEntry{3, tr("消息管理器"), tr("查看、清空聊天记录"),   ":/resource/images/ic_chat.png"}
         << AppEntry{4, tr("文件助手"),   tr("打开接收文件目录"),     ":/resource/images/ic_file.png"}
         << AppEntry{5, tr("修改密码"),   tr("修改登录密码"),         ":/resource/images/ic_mainsetting.png"}
         << AppEntry{6, tr("关于"),       tr("版本与账号信息"),       ":/resource/images/ic_info.png"};

    for (const AppEntry &app : apps) {
        QQCell *cell = new QQCell;
        cell->id = app.id;
        cell->name = app.name;
        cell->subTitle = app.desc;
        cell->iconPath = app.icon;
        cell->groupName = group->name;
        cell->type = QQCellType_Child;
        // 列表只渲染状态为在线/离线的子项，这里统一标成在线，否则条目会被过滤掉
        cell->status = OnLine;
        ui->widget_5->insertQQCell(cell);
    }
}

void MainWindow::SltAppCellClicked(QQCell *cell)
{
    if (nullptr == cell) {
        return;
    }

    const int id = cell->id;
    // 双击事件里直接弹窗口没问题，但统一延后一拍，避免列表重绘时控件被删
    QTimer::singleShot(0, this, [this, id]() {
        switch (id) {
        case 1: startWeather();        break;
        case 2: showCalendar();        break;
        case 3: openMessageManager();  break;
        case 4: openFileHelper();      break;
        case 5: openChangePasswd();    break;
        case 6: openAbout();           break;
        default: break;
        }
    });
}

void MainWindow::on_tbtnCalendar_clicked()
{
    showCalendar();
}

/**
 * @brief MainWindow::showCalendar
 * 日历：选日期看星期，双击回到今天
 */
void MainWindow::showCalendar()
{
    QDialog *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("日历"));
    dlg->setWindowIcon(QIcon(":/resource/common/ic_calendar.png"));

    QCalendarWidget *calendar = new QCalendarWidget(dlg);
    calendar->setGridVisible(true);
    calendar->setFirstDayOfWeek(Qt::Monday);
    calendar->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);

    QLabel *labelDate = new QLabel(dlg);
    labelDate->setAlignment(Qt::AlignCenter);
    labelDate->setStyleSheet("font:bold 13px '微软雅黑';color:#387ECC;");

    const QStringList weekNames = { tr("星期一"), tr("星期二"), tr("星期三"),
                                    tr("星期四"), tr("星期五"), tr("星期六"), tr("星期日") };
    auto refreshLabel = [calendar, labelDate, weekNames](const QDate &date) {
        labelDate->setText(QString("%1  %2")
                           .arg(date.toString("yyyy年MM月dd日"))
                           .arg(weekNames.value(date.dayOfWeek() - 1)));
    };
    refreshLabel(calendar->selectedDate());
    connect(calendar, &QCalendarWidget::selectionChanged, this, [=]() {
        refreshLabel(calendar->selectedDate());
    });

    QPushButton *btnToday = new QPushButton(tr("回到今天"), dlg);
    btnToday->setFixedHeight(28);
    connect(btnToday, &QPushButton::clicked, this, [=]() {
        calendar->setSelectedDate(QDate::currentDate());
        calendar->setCurrentPage(QDate::currentDate().year(), QDate::currentDate().month());
    });

    QVBoxLayout *layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->addWidget(labelDate);
    layout->addWidget(calendar, 1);
    layout->addWidget(btnToday);

    dlg->resize(380, 340);
    myHelper::FormInCenter(dlg);
    dlg->show();
}

/**
 * @brief MainWindow::openServerConfig
 * 系统设置：改服务器地址/端口，保存后立即重连
 */
void MainWindow::openServerConfig()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("系统设置"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setFixedWidth(380);

    QLineEdit *editIp = new QLineEdit(MyApp::m_strHostAddr, &dlg);
    QLineEdit *editMsgPort = new QLineEdit(QString::number(MyApp::m_nMsgPort), &dlg);
    QLineEdit *editFilePort = new QLineEdit(QString::number(MyApp::m_nFilePort), &dlg);
    editIp->setPlaceholderText("192.168.1.100");

    QFormLayout *form = new QFormLayout;
    form->addRow(tr("服务器地址："), editIp);
    form->addRow(tr("消息端口："), editMsgPort);
    form->addRow(tr("文件端口："), editFilePort);

    QLabel *hint = new QLabel(tr("保存后会断开当前连接并按新地址重连服务器"), &dlg);
    hint->setStyleSheet("color:#888888;font-size:12px;");

    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    box->button(QDialogButtonBox::Save)->setText(tr("保存并重连"));
    box->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    QVBoxLayout *layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addWidget(box);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    const QString ip = editIp->text().trimmed();
    if (!myHelper::IsIP(ip)) {
        CMessageBox::Warning(this, tr("服务器地址格式不正确，例如 192.168.1.100"), tr("系统设置"));
        return;
    }
    bool ok = false;
    const int msgPort = editMsgPort->text().trimmed().toInt(&ok);
    if (!ok || msgPort <= 0 || msgPort > 65535) {
        CMessageBox::Warning(this, tr("消息端口必须是 1~65535 之间的数字"), tr("系统设置"));
        return;
    }
    const int filePort = editFilePort->text().trimmed().toInt(&ok);
    if (!ok || filePort <= 0 || filePort > 65535) {
        CMessageBox::Warning(this, tr("文件端口必须是 1~65535 之间的数字"), tr("系统设置"));
        return;
    }

    MyApp::m_strHostAddr = ip;
    MyApp::m_nMsgPort = static_cast<quint16>(msgPort);
    MyApp::m_nFilePort = static_cast<quint16>(filePort);
    MyApp::SaveConfig();
    updateTrayTip();

    reconnectServer();
}

/**
 * @brief MainWindow::openMessageManager
 * 消息管理器：列出最近会话，可清空单个或全部聊天记录
 */
void MainWindow::openMessageManager()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("消息管理器"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.resize(620, 480);

    QLabel *title = new QLabel(tr("聊天记录（双击可直接打开会话窗口）"), &dlg);
    title->setStyleSheet("font:bold 13px '微软雅黑';color:#387ECC;");

    // 全局搜索：跨私聊 / 群聊检索，输入即搜，清空回到会话列表
    QLineEdit *editSearch = new QLineEdit(&dlg);
    editSearch->setPlaceholderText(tr("搜索所有会话的聊天记录…（输入关键字即时检索）"));
    editSearch->setClearButtonEnabled(true);

    QListWidget *list = new QListWidget(&dlg);
    list->setAlternatingRowColors(true);
    list->setStyleSheet("QListWidget{font:12px '微软雅黑';}");

    // 把会话列表填回去（搜索框清空时用）
    auto fillSessions = [&]() {
        list->clear();
        title->setText(tr("聊天记录（双击可直接打开会话窗口）"));
        title->setStyleSheet("font:bold 13px '微软雅黑';color:#387ECC;");

        const QVector<QJsonObject> sessions = DatabaseMsg::getInstance()->getRecentSessions();
        for (const QJsonObject &s : sessions) {
            const bool isGroup = s.value("isGroup").toBool();
            QListWidgetItem *item = new QListWidgetItem(list);
            item->setText(QString("%1%2\n   最后消息：%3    %4")
                          .arg(s.value("name").toString())
                          .arg(isGroup ? tr("（群）") : QString())
                          .arg(s.value("content").toString().left(40))
                          .arg(s.value("datetime").toString()));
            item->setData(Qt::UserRole, s.value("id").toInt());
            item->setData(Qt::UserRole + 1, isGroup);
            item->setData(Qt::UserRole + 2, s.value("name").toString());
            item->setIcon(QIcon(MyApp::HeadFullPath(s.value("head").toString())));
        }

        if (sessions.isEmpty()) {
            QListWidgetItem *item = new QListWidgetItem(tr("暂无聊天记录"), list);
            item->setFlags(Qt::NoItemFlags);
        }
    };

    // 关键字检索：结果里带上"哪个会话 / 谁说的 / 什么时候"
    auto doSearch = [&](const QString &keyword) {
        const QString kw = keyword.trimmed();
        if (kw.isEmpty()) {
            fillSessions();
            return;
        }

        list->clear();
        const QVector<QJsonObject> hits = DatabaseMsg::getInstance()->searchAllMessages(kw);
        title->setText(tr("搜索「%1」：命中 %2 条（双击进入会话）").arg(kw).arg(hits.size()));
        title->setStyleSheet("font:bold 13px '微软雅黑';color:#1a7f37;");

        for (const QJsonObject &m : hits) {
            const bool isGroup = m.value("isGroup").toBool();
            QString sessionName = m.value("sessionName").toString();
            if (sessionName.isEmpty()) sessionName = QString::number(m.value("sessionId").toInt());

            QString sender = m.value("sender").toString();
            if (m.value("direction").toInt() == Right) sender = tr("我");

            // 图片/文件/语音存的不是可读文本（本地路径或 b64），统一换成占位描述
            const int hitType = m.value("type").toInt();
            QString digest = m.value("content").toString();
            if (Face == hitType)         digest = tr("[动画表情]");
            else if (Picture == hitType) digest = tr("[图片]");
            else if (Files == hitType)   digest = tr("[文件] %1").arg(QFileInfo(digest).fileName());
            else if (Audio == hitType)   digest = tr("[语音] %1\"").arg(qMax(1, m.value("duration").toInt() / 1000));
            else                         digest = digest.left(60);

            QListWidgetItem *item = new QListWidgetItem(list);
            item->setText(QString("[%1] %2 · %3    %4\n   %5")
                          .arg(isGroup ? tr("群") : tr("私聊"))
                          .arg(sessionName)
                          .arg(sender)
                          .arg(m.value("datetime").toString())
                          .arg(digest));
            item->setData(Qt::UserRole, m.value("sessionId").toInt());
            item->setData(Qt::UserRole + 1, isGroup);
            item->setData(Qt::UserRole + 2, sessionName);

            const QString head = m.value("head").toString();
            if (!head.isEmpty()) item->setIcon(QIcon(MyApp::HeadFullPath(head)));
        }

        if (hits.isEmpty()) {
            QListWidgetItem *item = new QListWidgetItem(tr("没有找到包含「%1」的聊天记录").arg(kw), list);
            item->setFlags(Qt::NoItemFlags);
        }
    };

    fillSessions();

    QPushButton *btnOpen = new QPushButton(tr("打开会话"), &dlg);
    QPushButton *btnClearOne = new QPushButton(tr("清空该会话"), &dlg);
    QPushButton *btnClearAll = new QPushButton(tr("清空全部记录"), &dlg);
    QPushButton *btnClose = new QPushButton(tr("关闭"), &dlg);

    QHBoxLayout *btnLayout = new QHBoxLayout;
    btnLayout->addWidget(btnOpen);
    btnLayout->addWidget(btnClearOne);
    btnLayout->addWidget(btnClearAll);
    btnLayout->addStretch();
    btnLayout->addWidget(btnClose);

    QVBoxLayout *layout = new QVBoxLayout(&dlg);
    layout->addWidget(title);
    layout->addWidget(editSearch);
    layout->addWidget(list, 1);
    layout->addLayout(btnLayout);

    connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(editSearch, &QLineEdit::textChanged, this, [&](const QString &text) {
        if (text.trimmed().isEmpty()) doSearch(QString());
    });
    connect(editSearch, &QLineEdit::returnPressed, this, [&]() {
        doSearch(editSearch->text());
    });

    // 双击 = 打开会话
    connect(list, &QListWidget::itemDoubleClicked, this, [&](QListWidgetItem *item) {
        if (nullptr == item) {
            return;
        }
        QQCell *cell = new QQCell;
        cell->id = item->data(Qt::UserRole).toInt();
        cell->name = item->data(Qt::UserRole + 2).toString();
        cell->isGroup = item->data(Qt::UserRole + 1).toBool();
        cell->groupName = tr("最近会话");
        cell->type = QQCellType_Child;
        openChatWindow(cell, cell->isGroup, true);
    });

    // "打开会话"按钮与双击同义
    connect(btnOpen, &QPushButton::clicked, this, [&]() {
        if (QListWidgetItem *item = list->currentItem()) {
            emit list->itemDoubleClicked(item);
        }
    });

    // 清空单个会话
    connect(btnClearOne, &QPushButton::clicked, this, [&]() {
        QListWidgetItem *item = list->currentItem();
        if (nullptr == item || item->flags() == Qt::NoItemFlags) {
            CMessageBox::Infomation(dlg.parentWidget(), tr("请先在列表里选择一个会话"), tr("消息管理器"));
            return;
        }
        const int id = item->data(Qt::UserRole).toInt();
        const bool isGroup = item->data(Qt::UserRole + 1).toBool();
        if (CMessageBox::Question(&dlg,
                                  tr("确定要清空「%1」的全部聊天记录吗？\n此操作不可恢复。")
                                  .arg(item->data(Qt::UserRole + 2).toString()),
                                  tr("清空聊天记录")) != 1) {
            return;
        }
        DatabaseMsg::getInstance()->clearSessionMessages(id, isGroup);
        refreshConversationList();
        refreshUnreadBadges();
        doSearch(editSearch->text());   // 清完重新跑一次当前查询
    });

    // 清空全部
    connect(btnClearAll, &QPushButton::clicked, this, [&]() {
        if (CMessageBox::Question(&dlg, tr("确定要清空所有会话的聊天记录吗？\n此操作不可恢复。"),
                                  tr("清空全部记录")) != 1) {
            return;
        }
        const QVector<QJsonObject> sessions = DatabaseMsg::getInstance()->getRecentSessions();
        for (const QJsonObject &s : sessions) {
            DatabaseMsg::getInstance()->clearSessionMessages(s.value("id").toInt(),
                                                             s.value("isGroup").toBool());
        }
        fillSessions();
        refreshConversationList();
        refreshUnreadBadges();
        notifyTray(tr("消息管理器"), tr("已清空全部聊天记录"));
    });

    dlg.exec();
}

/**
 * @brief MainWindow::openFileHelper
 * 文件助手：打开接收文件目录
 */
void MainWindow::openFileHelper()
{
    QDir dir(MyApp::m_strRecvPath);
    if (!dir.exists() && !dir.mkpath(".")) {
        CMessageBox::Warning(this, tr("接收目录不存在，也无法创建：\n%1").arg(MyApp::m_strRecvPath),
                             tr("文件助手"));
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(MyApp::m_strRecvPath));
    notifyTray(tr("文件助手"), tr("已打开接收文件目录"));
}

/**
 * @brief MainWindow::openChangePasswd
 * 修改密码：校验后发给服务器，由服务器确认
 */
void MainWindow::openChangePasswd()
{
    if (m_tcpSocket->state() != QAbstractSocket::ConnectedState) {
        CMessageBox::Warning(this, tr("当前未连接服务器，无法修改密码"), tr("修改密码"));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("修改密码"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setFixedWidth(340);

    QLineEdit *editOld = new QLineEdit(&dlg);
    QLineEdit *editNew = new QLineEdit(&dlg);
    QLineEdit *editConfirm = new QLineEdit(&dlg);
    editOld->setEchoMode(QLineEdit::Password);
    editNew->setEchoMode(QLineEdit::Password);
    editConfirm->setEchoMode(QLineEdit::Password);
    editOld->setPlaceholderText(tr("请输入当前密码"));
    editNew->setPlaceholderText(tr("至少 6 位"));
    editConfirm->setPlaceholderText(tr("再输入一次新密码"));

    QFormLayout *form = new QFormLayout;
    form->addRow(tr("原密码："), editOld);
    form->addRow(tr("新密码："), editNew);
    form->addRow(tr("确认新密码："), editConfirm);

    QLabel *hint = new QLabel(tr("密码长度不少于 6 位，且不能与原密码相同"), &dlg);
    hint->setStyleSheet("color:#888888;font-size:12px;");

    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    box->button(QDialogButtonBox::Ok)->setText(tr("确定"));
    box->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    QVBoxLayout *layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addWidget(box);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    const QString oldPwd = editOld->text();
    const QString newPwd = editNew->text();
    if (oldPwd.isEmpty()) {
        CMessageBox::Warning(this, tr("请输入原密码"), tr("修改密码"));
        return;
    }
    if (newPwd.length() < 6) {
        CMessageBox::Warning(this, tr("新密码长度不能少于 6 位"), tr("修改密码"));
        return;
    }
    if (newPwd != editConfirm->text()) {
        CMessageBox::Warning(this, tr("两次输入的新密码不一致"), tr("修改密码"));
        return;
    }
    if (newPwd == oldPwd) {
        CMessageBox::Warning(this, tr("新密码不能与原密码相同"), tr("修改密码"));
        return;
    }

    QJsonObject obj;
    obj.insert("old", oldPwd);
    obj.insert("new", newPwd);
    m_strPendingPasswd = newPwd;
    m_tcpSocket->sendMessage(ChangePasswd, obj);
    notifyTray(tr("修改密码"), tr("已提交，正在等待服务器确认..."));
}

/**
 * @brief MainWindow::openAbout
 * 关于
 */
void MainWindow::openAbout()
{
    const QString online = (m_tcpSocket->state() == QAbstractSocket::ConnectedState)
            ? tr("在线") : tr("离线");
    CMessageBox::Infomation(this,
                            QString("%1\n\n局域网即时通讯客户端\n"
                                    "当前账号：%2（ID:%3）\n"
                                    "登录状态：%4\n"
                                    "服务器：%5:%6")
                            .arg(QString("畅想聊天APP  v%1").arg(MY_VERSION_STR))
                            .arg(MyApp::m_strUserName)
                            .arg(MyApp::m_nId)
                            .arg(online)
                            .arg(MyApp::m_strHostAddr)
                            .arg(MyApp::m_nMsgPort),
                            tr("关于"));
}

/**
 * @brief MainWindow::sltServerReconnected
 * 断线后链路恢复：服务器此时把本连接当成"未登录"的新连接，
 * 必须重新走一次 Login，否则收不到消息推送、离线队列也不会下发。
 */
void MainWindow::sltServerReconnected()
{
    qInfo() << "[重连] 链路已恢复，准备重新登录" << MyApp::m_strUserName;
    m_nReloginTries = 0;

    if (MyApp::m_strUserName.isEmpty() || MyApp::m_strPassword.isEmpty()) {
        // 没勾"自动登录"时内存里也留着口令，一般不会走到这里
        setConnState(false, tr("需要重新登录"));
        notifyTray(tr("连接已恢复"), tr("请重新登录以恢复消息收发"));
        return;
    }

    sltRelogin();
}

/**
 * @brief MainWindow::sltRelogin
 * 发一次登录请求，并起超时定时器；没回执就判定这次没成
 */
void MainWindow::sltRelogin()
{
    if (!m_tcpSocket || !m_tcpSocket->isConnected()) return;
    if (MyApp::m_strUserName.isEmpty() || MyApp::m_strPassword.isEmpty()) return;

    QJsonObject json;
    json.insert("name", MyApp::m_strUserName);
    json.insert("passwd", MyApp::m_strPassword);
    m_tcpSocket->sendMessage(Login, QJsonValue(json));

    qInfo() << "[重连] 正在重新登录（第" << (m_nReloginTries + 1) << "次）";
    m_reloginTimer->start();
}

void MainWindow::sltReloginTimeout()
{
    ++m_nReloginTries;

    if (m_nReloginTries >= 6) {
        qWarning() << "[重连] 自动重新登录多次未成功，放弃";
        setConnState(false, tr("自动登录失败，请手动重连"));
        notifyTray(tr("重新登录失败"),
                   tr("请检查网络或服务器后，在托盘菜单点\"我在线上\"重新连接"));
        return;
    }

    // 服务器可能还没把上一次的会话清掉（心跳超时最多 60 秒），隔一会儿再试
    qInfo() << "[重连] 没收到登录回执，第" << (m_nReloginTries + 1) << "次重试";
    QTimer::singleShot(5000, this, &MainWindow::sltRelogin);
}

/**
 * @brief MainWindow::setConnState
 * 连接状态变化时更新托盘提示和窗口标题，让用户一眼看出"掉线了正在重连"
 */
void MainWindow::setConnState(bool online, const QString &detail)
{
    updateTrayTip();

    if (online) {
        setWindowTitle(QString("%1 - 畅想聊天").arg(MyApp::m_strUserName));
        if (!detail.isEmpty()) notifyTray(tr("已恢复连接"), detail);
    } else {
        QString title = QString("%1 - 畅想聊天（离线）").arg(MyApp::m_strUserName);
        if (!detail.isEmpty()) title += " · " + detail;
        setWindowTitle(title);
    }
}

/**
 * @brief MainWindow::openGroupMembers
 * 打开群成员管理窗口（同一个群只开一个，重复点击就把已有窗口提到前面）
 */
void MainWindow::openGroupMembers(QQCell *cell)
{
    if (nullptr == cell || cell->id <= 0) return;

    if (!m_tcpSocket || !m_tcpSocket->isConnected()) {
        CMessageBox::Warning(this, tr("当前未连接服务器，无法查看群成员"), tr("群成员"));
        return;
    }

    const int groupId = cell->id;
    GroupMemberDialog *dlg = m_groupMemberDialogs.value(groupId, nullptr);

    if (nullptr == dlg) {
        dlg = new GroupMemberDialog(groupId, cell->name, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);

        connect(dlg, &GroupMemberDialog::signalRequestMembers, this, [this](int gid) {
            QJsonObject json;
            json.insert("groupId", gid);
            m_tcpSocket->sendMessage(GetGroupMembers, QJsonValue(json));
        });
        connect(dlg, &GroupMemberDialog::signalKickMember, this, [this](int gid, int userId) {
            QJsonObject json;
            json.insert("groupId", gid);
            json.insert("userId", userId);
            m_tcpSocket->sendMessage(KickGroupMember, QJsonValue(json));
        });
        connect(dlg, &GroupMemberDialog::signalTransferOwner, this, [this](int gid, int userId) {
            QJsonObject json;
            json.insert("groupId", gid);
            json.insert("userId", userId);
            m_tcpSocket->sendMessage(TransferGroupOwner, QJsonValue(json));
        });
        connect(dlg, &QObject::destroyed, this, [this, groupId]() {
            m_groupMemberDialogs.remove(groupId);
        });

        m_groupMemberDialogs.insert(groupId, dlg);
    }

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

/**
 * @brief MainWindow::reconnectServer
 * 手动重连服务器
 */
void MainWindow::reconnectServer()
{
    const QString ip = MyApp::m_strHostAddr;
    const quint16 port = MyApp::m_nMsgPort;
    if (ip.isEmpty() || 0 == port) {
        CMessageBox::Warning(this, tr("还没有配置服务器地址"), tr("连接服务器"));
        return;
    }

    m_tcpSocket->connectToHost(ip, port);
    updateTrayTip();
    notifyTray(tr("连接服务器"), QString("正在连接 %1:%2 ...").arg(ip).arg(port));
}

/**
 * @brief MainWindow::checkUpgrade
 * 升级：本机部署版没有升级服务器，这里只做版本提示
 */
void MainWindow::checkUpgrade()
{
    CMessageBox::Infomation(this,
                            QString("当前版本：v%1\n\n已是最新版本，无需升级。").arg(MY_VERSION_STR),
                            tr("检查更新"));
}

/**
 * @brief MainWindow::on_tbtnWeather_clicked
 * 点击天气按钮：打开同目录下的 weather.exe
 */
void MainWindow::on_tbtnWeather_clicked()
{
    startWeather();
}

void MainWindow::startWeather()
{
    if (nullptr == m_weatherProcess) {
        m_weatherProcess = new QProcess(this);
    }

    // 已经开着就不再重复启动
    if (m_weatherProcess->state() != QProcess::NotRunning) {
        qDebug() << "天气程序已在运行，不重复启动";
        return;
    }

    // 默认程序和主程序在同一目录（debug 目录），找不到再退回当前工作目录
    QString exePath = QCoreApplication::applicationDirPath() + "/weather/weather.exe";
    if (!QFile::exists(exePath)) {
        exePath = QDir::currentPath() + "/weather.exe";
    }
    if (!QFile::exists(exePath)) {
        CMessageBox::Warning(this,
                             QString("未找到天气程序 weather.exe，请把它放到程序目录下：\n%1")
                             .arg(QCoreApplication::applicationDirPath() + "/weather/"),
                             "天气查询");
        return;
    }

    // 工作目录设为 exe 所在目录，避免天气程序找不到自己的资源文件
    m_weatherProcess->setWorkingDirectory(QFileInfo(exePath).absolutePath());
    m_weatherProcess->start(exePath, QStringList());

    if (!m_weatherProcess->waitForStarted(3000)) {
        CMessageBox::Warning(this,
                             QString("天气程序启动失败：%1").arg(m_weatherProcess->errorString()),
                             "天气查询");
        return;
    }

    qDebug() << "天气程序已启动：" << exePath;
}

void MainWindow::stopWeather()
{
    if (nullptr == m_weatherProcess) {
        return;
    }
    if (m_weatherProcess->state() == QProcess::NotRunning) {
        return;
    }

    qDebug() << "正在关闭天气程序...";
    // terminate 在 Windows 下是给它的窗口发 WM_CLOSE，让它自己正常退出
    m_weatherProcess->terminate();
    if (!m_weatherProcess->waitForFinished(3000)) {
        // 3 秒还没退出（比如弹了确认框）就直接杀掉
        m_weatherProcess->kill();
        m_weatherProcess->waitForFinished(1000);
    }
    qDebug() << "天气程序已关闭";
}

void MainWindow::setHead(const QString &headFile)
{
    QString absPath = QFileInfo(headFile).absoluteFilePath();
    QPixmap pix(headFile);
    if (pix.isNull()) {
        qWarning() << "头像图片加载失败：" << headFile;
        return;
    }
    QString qss = QString("QWidget#widgetHead { "
                          "background-image: url(%1); "
                          "background-repeat: no-repeat; "
                          "background-position: center; "
                          "background-size: cover; "
                          "border-radius: 5px}")
            .arg(absPath);
    ui->widgetHead->setStyleSheet(qss);
    ui->widgetHead->setAutoFillBackground(true);
    ui->widgetHead->update();

}

void MainWindow::onChildPopMenuDidSelected(QAction *action)
{
    QQCell *cell = ui->frindListWidget->GetRightClickedCell();
    if (nullptr == cell) return;
    if(!action->text().compare(tr("发送即时消息")))
    {
        qDebug() << "send message" << cell->id;
        SltFriendsClicked(cell);

    } else if (!action->text().compare(tr("删除联系人"))) {
        qDebug() << "删除联系人" << cell->id;
        bool isDelete = DatabaseMsg::getInstance()->deleteFriend(cell->id);
        if(isDelete){
            CMessageBox::Infomation(this, "删除好友成功", "删除好友");
            QJsonObject jsonObj;
            jsonObj.insert("id", MyApp::m_nId);
            jsonObj.insert("to", cell->id);
            m_tcpSocket->sendMessage(DeleteFriend, QJsonValue(jsonObj));
        }else{
            CMessageBox::Warning(this, "删除好友失败", "删除好友");
        }
    }
}

void MainWindow::onGroupPopMenuDidSelected(QAction *action)
{
    if(!action->text().compare(tr("创建讨论组")))
    {
        QString text = CInputDialog::GetInputText(this, "群组");
        if (!text.isEmpty()) {

            // 构建 Json 对象
            QJsonObject json;
            json.insert("id", m_tcpSocket->GetUserId());
            json.insert("name", text);

            m_tcpSocket->sendMessage(CreateGroup, json);
        }
    }
    if(!action->text().compare(tr("加入讨论组")))
    {
        QString id = CInputDialog::GetInputText(this, "1","请输入群号:");
        if(!id.isEmpty()) {
            QJsonObject json;
            json.insert("id", MyApp::m_nId);
            json.insert("groupId", id.toInt());
            m_tcpSocket->sendMessage(AddGroupRequist, json);
        }
    }
    if(!action->text().compare(tr("刷新"))){
        QJsonObject jsonObj;
        jsonObj.insert("id",MyApp::m_nId);

        m_tcpSocket->sendMessage(RefreshGroups, QJsonValue(jsonObj));
    }
}


void MainWindow::onAddFriendMenuDidSelected(QAction *action)
{
    if (!action->text().compare(tr("添加好友")))
    {
        QString name = CInputDialog::GetInputText(this, "zhangsan");

        if(name.isEmpty())
        {
            return;
        }
        if(name == MyApp::m_strUserName){
            CMessageBox::Warning(this, "不能添加自己为好友","添加好友");
            return;
        }
        // 首先判断该用户是否已经是我的好友了
        if (DatabaseMsg::getInstance()->isMyFriend(name)) {
            CMessageBox::Infomation(this, "该用户已经是你的好友了！","添加好友");
            return;
        }
        // 构建 Json 对象
        QJsonObject json;
        json.insert("id", m_tcpSocket->GetUserId());
        json.insert("name", name);

        m_tcpSocket->sendMessage(AddFriend, json);
    }
    else if (!action->text().compare(tr("刷新")))
    {
        QJsonObject jsonObj;
        jsonObj.insert("id",MyApp::m_nId);

        // 组织Jsonarror
        m_tcpSocket->sendMessage(RefreshFriends, QJsonValue(jsonObj));
    }
    else if (!action->text().compare(tr("删除该组")))
    {
        qDebug() << "delete group";
    }
}

void MainWindow::SltFriendsClicked(QQCell *cell)
{
    for(ChatWindow *window : m_chatFriendWindows){
        if(window->getUserId() == cell->id){
            qDebug() << cell->id;
            window->show();
            window->raise();    //置于最顶层
            DatabaseMsg::getInstance()->markSessionRead(cell->id, false);
            refreshUnreadBadges();
            refreshConversationList();
            return;
        }
    }

    openChatWindow(cell, false, false);
}

/**
 * @brief MainWindow::openChatWindow
 * 打开（或复用）一个聊天窗口
 * @param ownCell 为 true 时窗口接管 cell 的内存（会话列表里的临时 cell）
 */
ChatWindow *MainWindow::openChatWindow(QQCell *cell, bool isGroup, bool ownCell)
{
    ChatWindow *chatWindow = new ChatWindow();
    if (isGroup) {
        chatWindow->SetIsGroup();
    }

    connect(chatWindow,&ChatWindow::signalSendMessage, this, &MainWindow::sltChatSendMessage);
    connect(chatWindow,&ChatWindow::signalClose, this,
            isGroup ? &MainWindow::SltGroupChatWindowClose : &MainWindow::SltFriendChatWindowClose);
    connect(chatWindow,&ChatWindow::signalSessionChanged, this, [=](){
        refreshConversationList();
        refreshUnreadBadges();
    });

    if (ownCell) {
        chatWindow->setCellOwned(cell);
    } else {
        chatWindow->setCell(cell);
    }
    chatWindow->show();

    // 打开会话就把对方（或群成员）的公钥拉齐，发消息时不用等
    prefetchChatKeys(chatWindow, isGroup, cell ? cell->id : -1);

    if (isGroup) {
        m_chatGroupWindows.append(chatWindow);
    } else {
        m_chatFriendWindows.append(chatWindow);
    }
    return chatWindow;
}

void MainWindow::sltStatus(const quint8 &status, const QJsonValue &dataVal)
{
    switch (status) {
    // ===== 断线重连：重新登录的应答 =====
    case LoginSuccess:
    {
        if (m_reloginTimer) m_reloginTimer->stop();
        m_nReloginTries = 0;

        const QJsonObject obj = dataVal.toObject();
        const int newId = obj.value("id").toInt();
        if (newId > 0) MyApp::m_nId = newId;

        qInfo() << "[重连] 已重新登录成功，id =" << MyApp::m_nId;
        setConnState(true, tr("连接已恢复，消息收发正常"));

        // 服务器紧接着会推好友、群组和离线消息，界面会自己刷新；
        // 这里先重算一次会话列表，避免断线期间的未读统计对不上
        refreshConversationList();
        refreshUnreadBadges();
        break;
    }
    case LoginRepeat:
    {
        // 服务器还认为这个账号在线（断线时它还没超时把旧连接清掉），
        // 等它踢掉旧会话再重试，最多 6 次
        if (m_reloginTimer) m_reloginTimer->stop();
        ++m_nReloginTries;
        if (m_nReloginTries < 6) {
            qInfo() << "[重连] 账号仍被判为在线，5 秒后重试登录";
            QTimer::singleShot(5000, this, &MainWindow::sltRelogin);
        } else {
            setConnState(false, tr("账号在别处登录或旧会话未释放"));
        }
        break;
    }
    // ===== 端到端加密：公钥交换 =====
    case UploadKeysOk:
    {
        m_bCryptoReady = true;
        qInfo() << "[加密] 公钥已登记到服务器，可以开始端到端加密";
        sltFlushPendingMessages();
        break;
    }
    case PubKeyData:
    {
        const QJsonObject obj = dataVal.toObject();
        const QJsonObject keys = obj.value("keys").toObject();
        for (QJsonObject::const_iterator it = keys.constBegin(); it != keys.constEnd(); ++it) {
            KeyStore::instance()->cachePubKey(it.key().toInt(), it.value().toString());
            m_pendingKeyRequests.remove(it.key().toInt());
        }
        sltFlushPendingMessages();
        break;
    }
    case KeyOperateFailed:
    {
        const QJsonObject obj = dataVal.toObject();
        qWarning() << "[加密] 密钥操作失败：" << obj.value("msg").toString();
        break;
    }
    case AddFriendFailed_NoneUser:
    {
        CMessageBox::Warning(this,"用户名不存在","添加好友");
        break;
    }
    case AddFriendRequist:
    {
        addFriendRequist(dataVal);
        break;
    }
    case AddFriendReply:
    {
        addFriendReply(dataVal);    //收到回复
        break;
    }
    case GetMyFriends:
    {
        showServerFriendInfo(dataVal);  //从服务器获取好友信息
        break;
    }
    case RefreshFriends:
    {
        showServerFriendInfo(dataVal);
        break;
    }
    case SendMsg:
    {
        // 消息一到就解密，与聊天窗口是否打开无关
        deliverIncoming(SendMsg, dataVal);
        break;
    }
    case SendFace:
    {
        // 表情的 msg 里放的是表情下标，同样走端到端加密：
        // 必须经过 deliverIncoming 解密，否则拿到的是密文包，toInt() 恒为 0，
        // 接收方会永远显示第 1 张表情。
        deliverIncoming(SendFace, dataVal);
        break;
    }
    case SendFile:
    {
        // 文件消息的 msg 是文件名/大小等元信息，也已加密，同样先解密再显示
        deliverIncoming(SendFile, dataVal);
        break;
    }
    case DeleteFriend:
    {
        deleteFriend(dataVal);
        break;
    }
    case CreateGroup:
    {
        createGroup(dataVal);
        break;
    }
    case GetMyGroups:
    {
        showServerGroupsInfo(dataVal);
        break;
    }
    case RefreshGroups:
    {
        showServerGroupsInfo(dataVal);
        break;
    }
    case AddGroupRequist:
    {
        addGroupRequist(dataVal);
        break;
    }
    case AddGroupReply:
    {
        addGroupReply(dataVal);
        break;
    }
    case AddGroupAccept:
    {
        addGroupAccept(dataVal);
        break;
    }
    case SendGroupMsg:
    {
        deliverIncoming(SendGroupMsg, dataVal);
        break;
    }
    case RecallMsg:     // 对端撤回了某条消息
    {
        handleRecallMsg(dataVal);
        break;
    }
    case SendAck:       // 服务端确认已受理我发出的某条消息
    {
        handleSendAck(dataVal);
        break;
    }
    case QuitGroupOk:       // 自己退群成功
    {
        QJsonObject obj = dataVal.toObject();
        const int groupId = obj.value("groupId").toInt();
        DatabaseMsg::getInstance()->removeGroupMember(groupId, MyApp::m_nId);
        removeGroupCell(groupId);
        CMessageBox::Infomation(this, "已退出该群聊", "退出群组");
        break;
    }
    case DissolveGroupOk:   // 自己解散成功
    {
        QJsonObject obj = dataVal.toObject();
        const int groupId = obj.value("groupId").toInt();
        DatabaseMsg::getInstance()->removeGroup(groupId);
        removeGroupCell(groupId);
        CMessageBox::Infomation(this, "群聊已解散", "解散群聊");
        break;
    }
    case GroupDissolved:    // 群主解散了群，通知剩余成员
    {
        QJsonObject obj = dataVal.toObject();
        const int groupId = obj.value("groupId").toInt();
        const QString groupName = obj.value("name").toString();
        DatabaseMsg::getInstance()->removeGroup(groupId);
        removeGroupCell(groupId);
        CMessageBox::Infomation(this, QString("群聊'%1'已被群主解散").arg(groupName), "群组通知");
        break;
    }
    case GroupOperateFailed:
    {
        QJsonObject obj = dataVal.toObject();
        CMessageBox::Warning(this, obj.value("msg").toString("操作失败"), "群组操作");
        break;
    }
    // ===== 群成员管理 =====
    case GroupMembers:      // 成员列表（刚拉取/刷新完）
    {
        const QJsonObject obj = dataVal.toObject();
        const int groupId = obj.value("groupId").toInt();
        if (GroupMemberDialog *dlg = m_groupMemberDialogs.value(groupId, nullptr)) {
            dlg->setMembers(obj);
        }
        break;
    }
    case GroupMemberChanged:    // 有人被移出 / 群主变更
    {
        const QJsonObject obj = dataVal.toObject();
        const int groupId = obj.value("groupId").toInt();
        const int ownerId = obj.value("ownerId").toInt(-1);

        // 群主变了：同步本地角色，群列表右键菜单的"解散/退群"才会跟着变
        if (ownerId > 0) {
            DatabaseMsg::getInstance()->setGroupCreator(groupId, ownerId);
            DatabaseMsg::getInstance()->setGroupIdentity(
                        groupId, ownerId == MyApp::m_nId ? Owner : Member);
        }

        // 正在看的成员窗口同步刷新
        if (GroupMemberDialog *dlg = m_groupMemberDialogs.value(groupId, nullptr)) {
            QJsonObject req;
            req.insert("groupId", groupId);
            m_tcpSocket->sendMessage(GetGroupMembers, QJsonValue(req));
        }
        break;
    }
    case GroupKicked:       // 自己被群主移出
    {
        const QJsonObject obj = dataVal.toObject();
        const int groupId = obj.value("groupId").toInt();
        const QString groupName = obj.value("name").toString();

        DatabaseMsg::getInstance()->removeGroupMember(groupId, MyApp::m_nId);
        removeGroupCell(groupId);

        if (GroupMemberDialog *dlg = m_groupMemberDialogs.value(groupId, nullptr)) {
            dlg->close();
            m_groupMemberDialogs.remove(groupId);
        }

        notifyTray(tr("群聊通知"),
                   QString("你已被移出群聊'%1'").arg(groupName.isEmpty()
                                                    ? QString::number(groupId) : groupName));
        break;
    }
    case UpdateHeadOk:      // 自己换头像成功
    {
        QJsonObject obj = dataVal.toObject();
        const QString head = obj.value("head").toString();
        if (!head.isEmpty()) {
            MyApp::m_strHeadFile = head;
            setHead(MyApp::HeadFullPath(head));
        }
        qDebug() << "[头像] 上传成功，服务器文件名：" << head;
        break;
    }
    case HeadChanged:       // 别人换了头像
    {
        QJsonObject obj = dataVal.toObject();
        const int userId = obj.value("id").toInt();
        const QString head = obj.value("head").toString();
        if (userId <= 0 || userId == MyApp::m_nId || head.isEmpty()) {
            break;
        }
        qDebug() << "[头像] 用户" << userId << "换了头像：" << head;
        DatabaseMsg::getInstance()->updateFriendHead(userId, head);
        ensureHeadAvailable(userId, head);
        refreshHeadUi(userId);
        break;
    }
    case HeadData:          // 服务器下发的头像图片
    {
        QJsonObject obj = dataVal.toObject();
        const int userId = obj.value("id").toInt();
        const QString head = obj.value("head").toString();
        m_headRequests.remove(userId);

        const QString localPath = saveHeadImage(head, obj.value("data").toString());
        if (localPath.isEmpty()) {
            break;
        }

        if (userId == MyApp::m_nId) {
            MyApp::m_strHeadFile = head;
            setHead(localPath);
            qDebug() << "[头像] 已从服务器取回自己的头像：" << head;
        } else {
            DatabaseMsg::getInstance()->updateFriendHead(userId, head);
            refreshHeadUi(userId);
            qDebug() << "[头像] 已取回用户" << userId << "的头像：" << head;
        }
        break;
    }
    case HeadOperateFailed:
    {
        QJsonObject obj = dataVal.toObject();
        CMessageBox::Warning(this, obj.value("msg").toString("头像操作失败"), "头像");
        break;
    }
    case ChangePasswdOk:
    {
        // 本地也换成新密码，否则勾了"自动登录"下次会拿旧密码登不上
        if (!m_strPendingPasswd.isEmpty()) {
            MyApp::m_strPassword = m_strPendingPasswd;
            m_strPendingPasswd.clear();
            // 只动口令这一项：没勾"记住密码"时它会把 ini 里的旧口令删掉
            MyApp::SavePassword(MyApp::m_strPassword);
        }
        CMessageBox::Infomation(this, "密码修改成功，下次登录请使用新密码", "修改密码");
        break;
    }
    case ChangePasswdFailed:
    {
        QJsonObject obj = dataVal.toObject();
        CMessageBox::Warning(this, obj.value("msg").toString("密码修改失败"), "修改密码");
        break;
    }
    }
}


void MainWindow::SltFriendChatWindowClose()
{
    ChatWindow *chatWindow = (ChatWindow*)sender();

    disconnect(chatWindow,&ChatWindow::signalSendMessage, this,&MainWindow::sltChatSendMessage);
    disconnect(chatWindow,&ChatWindow::signalClose, this,&MainWindow::SltFriendChatWindowClose);

    if(!this->isVisible() && m_chatFriendWindows.size() == 1){
        this->show();
    }
    m_chatFriendWindows.removeOne(chatWindow);
}

void MainWindow::SltGroupChatWindowClose()
{
    ChatWindow *chatWindow = (ChatWindow*)sender();

    disconnect(chatWindow,&ChatWindow::signalSendMessage, this,&MainWindow::sltChatSendMessage);
    disconnect(chatWindow,&ChatWindow::signalClose, this,&MainWindow::SltGroupChatWindowClose);

    if(!this->isVisible() && m_chatGroupWindows.size() == 1){
        this->show();
    }
    m_chatGroupWindows.removeOne(chatWindow);
}

void MainWindow::SltGroupsClicked(QQCell *cell)
{
    for(ChatWindow *window : m_chatGroupWindows){
        if(window->getUserId() == cell->id){
            qDebug() << cell->id;
            window->show();
            window->raise();    //置于最顶层
            DatabaseMsg::getInstance()->markSessionRead(cell->id, true);
            refreshUnreadBadges();
            refreshConversationList();
            return;
        }
    }

    openChatWindow(cell, true, false);
}

void MainWindow::onGroupMenuDidSelected(QAction *action)
{
    qDebug() << "右键群组框";
    QQCell *cell = ui->groupListWidget->GetRightClickedCell();
    if (nullptr == cell) return;
    if(!action->text().compare(tr("发送即时消息")))
    {
        qDebug() << "group message" << cell->id;
        SltGroupsClicked(cell);
    }
    else if (!action->text().compare(tr("群成员")))
    {
        openGroupMembers(cell);
    }
    else if (!action->text().compare(tr("退出群组")))
    {
        int ret = CMessageBox::Question(this,
                                        QString("确定要退出群聊'%1'吗？").arg(cell->name),
                                        "退出群组");
        if (QDialog::Accepted != ret) return;

        QJsonObject jsonObj;
        jsonObj.insert("id", MyApp::m_nId);
        jsonObj.insert("groupId", cell->id);
        m_tcpSocket->sendMessage(QuitGroup, jsonObj);
    }
    else if (!action->text().compare(tr("解散群聊")))
    {
        int ret = CMessageBox::Question(this,
                                        QString("解散后所有成员都会被移出群聊，确定解散'%1'吗？").arg(cell->name),
                                        "解散群聊");
        if (QDialog::Accepted != ret) return;

        QJsonObject jsonObj;
        jsonObj.insert("id", MyApp::m_nId);
        jsonObj.insert("groupId", cell->id);
        m_tcpSocket->sendMessage(DissolveGroup, jsonObj);
    }
}

/**
 * @brief MainWindow::SltGroupMenuWillShow
 * 群组右键菜单弹出前：按身份显示"退出群组"或"解散群聊"
 */
void MainWindow::SltGroupMenuWillShow(QQListWidget *listWidget, QQPopMenuWidget *widget, QMenu *menu)
{
    Q_UNUSED(widget)
    if (menu == nullptr || listWidget != ui->groupListWidget) return;

    QQCell *cell = ui->groupListWidget->GetRightClickedCell();
    if (nullptr == cell || QQCellType_Child != cell->type) return;

    const int groupId = cell->id;
    const int identity = DatabaseMsg::getInstance()->getMyGroupIdentity(groupId);
    const int creator  = DatabaseMsg::getInstance()->getGroupCreator(groupId);
    const bool isOwner = (Owner == identity) || (creator == MyApp::m_nId);

    for (QAction *action : menu->actions())
    {
        if (!action->text().compare(tr("退出群组")))
        {
            action->setVisible(!isOwner);
        }
        else if (!action->text().compare(tr("解散群聊")))
        {
            action->setVisible(isOwner);
        }
    }
}

/**
 * @brief MainWindow::removeGroupCell
 * 退出/解散后从群组页移除该群，并关闭它的聊天窗口
 */
void MainWindow::removeGroupCell(int groupId)
{
    // 关闭已打开的群聊窗口（走正常关闭流程，窗口会把自己从列表里移除）
    for (int i = m_chatGroupWindows.size() - 1; i >= 0; --i) {
        ChatWindow *window = m_chatGroupWindows.at(i);
        if (window->getUserId() == groupId) {
            QMetaObject::invokeMethod(window, "on_btnWinClose_clicked");
        }
    }

    // 从群组列表里移除（cell 不 delete，避免窗口还在用它）
    const QList<QQCell *> groups = ui->groupListWidget->getCells();
    for (QQCell *group : groups) {
        for (QQCell *child : group->childs) {
            if (QQCellType_Child == child->type && child->id == groupId) {
                ui->groupListWidget->removeQQCell(child);
                break;
            }
        }
    }

    refreshConversationList();
    refreshUnreadBadges();
}

void MainWindow::addFriendRequist(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "添加好友转发有误";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();

    // 不再弹框打断用户，先入"申请列表"，由用户点 tbtnAddUser 统一处理
    PendingRequest req;
    req.type = AddFriendRequist;
    req.id   = jsonObj.value("id").toInt();
    req.name = jsonObj.value("name").toString();
    addPendingRequest(req);
}

void MainWindow::addFriendReply(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "添加好友回复出错";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
    int id = jsonObj.value("id").toInt();
    QString friendName = jsonObj.value("name").toString();
    QString msg = jsonObj.value("msg").toString();
    if(msg.compare("accept") == 0){
        DatabaseMsg::getInstance()->AddFriend(id,friendName,1);
        qDebug() << "对方已同意你的好友申请";
        return;
    }
    DatabaseMsg::getInstance()->removeFriend(friendName);
    qDebug() << "对方拒绝了你的好友申请";

    // 不弹模态框，托盘气泡提示一下即可
    notifyTray(tr("添加好友"), QString("'%1'拒绝了你的好友申请").arg(friendName));
}

void MainWindow::deleteFriend(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
    const int friendId = jsonObj.value("id").toInt();
    const QString friendName = DatabaseMsg::getInstance()->getFriendName(friendId);

    // 不弹模态框，用托盘气泡提示
    notifyTray(tr("删除好友"), QString("'%1'删除了你的好友").arg(friendName));

    DatabaseMsg::getInstance()->deleteFriend(friendId);

    // 关掉和该好友的聊天窗口，并从好友列表里移除
    for (int i = m_chatFriendWindows.size() - 1; i >= 0; --i) {
        if (m_chatFriendWindows.at(i)->getUserId() == friendId) {
            QMetaObject::invokeMethod(m_chatFriendWindows.at(i), "on_btnWinClose_clicked");
        }
    }
    const QList<QQCell *> groups = ui->frindListWidget->getCells();
    for (QQCell *group : groups) {
        for (QQCell *child : group->childs) {
            if (QQCellType_Child == child->type && child->id == friendId) {
                ui->frindListWidget->removeQQCell(child);
                break;
            }
        }
    }
    refreshUnreadBadges();
}

void MainWindow::showServerGroupsInfo(const QJsonValue &dataVal)
{
    if(!dataVal.isArray()){
        qDebug() << "获取好友信息通信有误";
        return;
    }
    QList<QQCell *> groups = ui->groupListWidget->getCells();
    if (!groups.isEmpty()) {
        QQCell* myGroup = groups.at(0);

        for (QQCell* oldChild : myGroup->childs) {
            if(oldChild) delete oldChild;
        }
        myGroup->childs.clear();
    }
    QJsonArray array = dataVal.toArray();
    int size = array.size();
    for(int i = 0;i < size; i++){
        QJsonObject jsonObj = array.at(i).toObject();
        QString strHead = jsonObj.value("head").toString();
        int owner = jsonObj.value("owner").toInt();
        QQCell* cell = new QQCell;
        cell->groupName = QString(tr("我的群组"));
        cell->iconPath = MyApp::HeadFullPath(strHead);
        cell->type = QQCellType_Child;
        cell->name = jsonObj.value("name").toString();
        owner == MyApp::m_nId ? cell->subTitle = QString("我的群，我做主...  群号：%1").arg(jsonObj.value("id").toInt())
                : cell->subTitle = QString("我加入的群  群号：%1").arg(jsonObj.value("id").toInt());
        cell->id = jsonObj.value("id").toInt();
        cell->status = OnLine;
        ui->groupListWidget->insertQQCell(cell);
    }
    ui->groupListWidget->upload();
}
void MainWindow::showServerFriendInfo(const QJsonValue &dataVal)
{
    if(!dataVal.isArray()){
        qDebug() << "获取好友信息通信有误";
        return;
    }
    //为了刷新出新添加的好友和头像灰度改变，先删除，再添加
    QList<QQCell *> groups = ui->frindListWidget->getCells();
    if (!groups.isEmpty()) {
        QQCell* myFriendGroup = groups.at(0); // 我的好友分组

        for (QQCell* oldChild : myFriendGroup->childs) {
            if(oldChild) delete oldChild;
        }
        myFriendGroup->childs.clear();
    }
    QJsonArray array = dataVal.toArray();
    int size = array.size();
    for(int i = 0;i < size; i++){
        QJsonObject jsonObj = array.at(i).toObject();
        int status = jsonObj.value("status").toInt();
        QString strHead = jsonObj.value("head").toString();
        const int friendId = jsonObj.value("id").toInt();

        // 记录好友最新头像；本地没有文件就向服务器拉
        DatabaseMsg::getInstance()->updateFriendHead(friendId, strHead);
        ensureHeadAvailable(friendId, strHead);

        QQCell* cell = new QQCell;
        cell->groupName = QString(tr("我的好友"));
        cell->iconPath = MyApp::HeadFullPath(strHead);
        cell->type = QQCellType_Child;
        cell->name = jsonObj.value("name").toString();
        cell->subTitle = QString("当前用户状态：%1").arg(OnLine == status ? tr("在线") : tr("离线"));
        cell->id = jsonObj.value("id").toInt();
        cell->status = status;
        //        qDebug() << "获取好友信息" << cell->id << cell->name;
        ui->frindListWidget->insertQQCell(cell);
    }

    ui->frindListWidget->upload();
}
void MainWindow::receiveMessage(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "消息解析失败";
        return;
    }

    QJsonObject jsonObj = dataVal.toObject();
    int id = jsonObj.value("id").toInt();   //好友的Id
    QString msg = jsonObj.value("msg").toString();
    QString head = jsonObj.value("head").toString();
    int type = jsonObj.value("type").toInt();
    // 兼容旧服务端：私聊转发曾丢掉 type，图片被当文字发下来，这里按内容前缀纠正
    if (static_cast<quint8>(type) == Text && msg.startsWith("b64:") && msg.size() > 256) {
        type = Picture;
    }
    qDebug() << "msg类型" << type << "长度" << msg.size();

    // 对方可能在离线期间换了头像：先同步头像再决定窗口里怎么显示
    if (!head.isEmpty()) {
        DatabaseMsg::getInstance()->updateFriendHead(id, head);
        ensureHeadAvailable(id, head);
    }

    foreach(ChatWindow* window, m_chatFriendWindows){
        if(window->getUserId() == id){
            window->AddMessage(dataVal);
            return;
        }
    }

    //下面写没有开启这个好友聊天窗口时的逻辑（已经包含上线转发）
    ItemInfo *itemInfo = new ItemInfo(this);
    itemInfo->SetName(DatabaseMsg::getInstance()->getFriendName(id));
    itemInfo->SetDatetime(QDateTime::currentDateTime().toString("MM-dd HH:mm"));
    itemInfo->SetHeadPixmap(MyApp::HeadFullPath(head));
    itemInfo->SetMsgType(static_cast<quint8>(type));
    itemInfo->SetTextColor(jsonObj.value("color").toString());
    if(static_cast<quint8>(type) == Text){
        itemInfo->SetText(msg);
    }else if(static_cast<quint8>(type) == Face){
        itemInfo->SetFace(msg.toInt());
    }else if(static_cast<quint8>(type) == Picture){
        // 图片消息：把内嵌的 base64 落盘，历史列表才能显示
        itemInfo->SetText(myHelper::SaveInlineImage(msg));
    }else if(static_cast<quint8>(type) == Audio){
        // 语音消息：同样先落盘成 wav，历史记录才能直接播放
        itemInfo->SetText(myHelper::SaveInlineAudio(msg));
        itemInfo->SetDuration(jsonObj.value("dur").toInt(0));
    }else if(static_cast<quint8>(type) == Files){
        itemInfo->SetText(msg);
        itemInfo->SetFileSizeString(jsonObj.value("size").toString());
    }
    itemInfo->SetMsgType(static_cast<quint8>(type));
    itemInfo->SetOrientation(Left);
    // 对端带过来的 key 必须存下来，撤回时才找得到这条
    const QString inKey = jsonObj.value("key").toString();
    itemInfo->SetMsgKey(inKey.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : inKey);
    itemInfo->SetTimeStamp(QDateTime::currentMSecsSinceEpoch());
    DatabaseMsg::getInstance()->AddHistoryMsg(id, itemInfo);

    // 消息没被打开（聊天窗口未开），算未读，刷新标签角标和会话列表
    refreshUnreadBadges();
    refreshConversationList();

    delete itemInfo;
    itemInfo = nullptr;

}

void MainWindow::createGroup(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "创建群组失败，服务器传输的数据有误";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
    QString name = jsonObj.value("name").toString();
    int groupId = jsonObj.value("groupId").toInt();
    DatabaseMsg::getInstance()->AddGroup(MyApp::m_nId, "default_group.png", name, groupId);
    DatabaseMsg::getInstance()->addGroupMember(MyApp::m_nId, groupId, Owner);
}

void MainWindow::addGroupRequist(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "请求添加好友失败，服务器传输数据有误";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();

    // 同样不弹框，放进申请列表
    PendingRequest req;
    req.type      = AddGroupRequist;
    req.id        = jsonObj.value("id").toInt();        // 申请人的Id
    req.name      = jsonObj.value("name").toString();   // 申请加群的用户名
    req.groupId   = jsonObj.value("groupId").toInt();
    req.groupName = DatabaseMsg::getInstance()->getGroupName(req.groupId);
    if (req.groupName.isEmpty()) {
        req.groupName = QString("群组%1").arg(req.groupId);
    }
    addPendingRequest(req);
}

void MainWindow::addGroupReply(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
        qDebug() << "添加好友回复有错误，服务器传输数据有误";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
    int groupId = jsonObj.value("groupId").toInt();
    QString name = jsonObj.value("name").toString();
    QString msg = jsonObj.value("msg").toString();
    QString groupName = jsonObj.value("groupName").toString();
    QString gropuHead = jsonObj.value("groupHead").toString();
    int id = jsonObj.value("userId").toInt();     //新人Id
    int ownerId = jsonObj.value("id").toInt();  //群主Id
    if(msg == QString("accept")) {
        if(MyApp::m_nId == id){
            // AddGroup 的第一个参数是"创建者id"，原来误传了群号，导致写入一条 group_id = 群主id 的错误记录
            DatabaseMsg::getInstance()->AddGroup(ownerId, gropuHead, groupName, groupId);

            // 不弹模态框，托盘气泡提示
            const QString groupText = groupName.isEmpty()
                    ? QString("群号 %1").arg(groupId)
                    : QString("'%1'（群号 %2）").arg(groupName).arg(groupId);
            notifyTray(tr("加入群组"), tr("你已加入群聊 %1").arg(groupText));
            qDebug() << "[群组] 已加入群聊" << groupId << groupName;

            // 让群组页立刻出现这个群，不用等下次登录
            QJsonObject refreshObj;
            refreshObj.insert("id", MyApp::m_nId);
            m_tcpSocket->sendMessage(RefreshGroups, QJsonValue(refreshObj));
        }
        DatabaseMsg::getInstance()->addGroupMember(id, groupId, Member);
    } else if(msg == QString("refuse")) {
        // 不弹模态框，托盘气泡提示
        const QString who = name.isEmpty() ? tr("对方") : name;
        notifyTray(tr("加入群组"), tr("'%1'拒绝了你的添加群组申请").arg(who));
        qDebug() << "[群组] 加群申请被拒绝" << groupId << who;
    }
}

void MainWindow::addGroupAccept(const QJsonValue &dataVal)
{
    if(!dataVal.isArray()){
        qDebug() << "接受群组申请传输数据有误(不是数组)，无法将群用户插入到客户端数据库";
        return;
    }
    QJsonArray jsonArr = dataVal.toArray();
    foreach(QJsonValue json, jsonArr){
        if(!json.isObject()){
             qDebug() << "接受群组申请传输数据有误(不是对象)，无法将群用户插入到客户端数据库";
             return;
        }
        QJsonObject jsonObj = json.toObject();
        int groupId = jsonObj.value("groupId").toInt();
        int userId = jsonObj.value("userId").toInt();
        int identity = jsonObj.value("identity").toInt();
        QString joinTime = jsonObj.value("joinTime").toString();
        DatabaseMsg::getInstance()->addGroupMember(userId, groupId, joinTime, static_cast<GroupIdentity>(identity));
    }
}

void MainWindow::sendGroupMsg(const QJsonValue &dataVal)
{
    if(!dataVal.isObject()){
            qDebug() << "消息解析失败";
            return;
        }
        QJsonObject jsonObj = dataVal.toObject();
        int groupId = jsonObj.value("id").toInt();   //群组的Id
        int userId = jsonObj.value("userId").toInt();
        QString userName = jsonObj.value("name").toString();
        QString msg = jsonObj.value("msg").toString();
        QString head = jsonObj.value("head").toString();
        int type = jsonObj.value("type").toInt();
        if(type == Face) msg = QString::number(jsonObj.value("msg").toInt());
        if(type == Files) groupId = jsonObj.value("to").toInt();
        qDebug() << "群消息类型" << type << "长度" << msg.size();

        // 群里可能是陌生人，头像文件本地没有就拉一次
        if (!head.isEmpty()) {
            ensureHeadAvailable(userId, head);
        }

        foreach(ChatWindow* window, m_chatGroupWindows){
            if(window->getUserId() == groupId){
                window->AddMessage(dataVal);
                return;
            }
        }

        //下面写没有开启这个好友聊天窗口时的逻辑（已经包含上线转发）
        ItemInfo *itemInfo = new ItemInfo(this);
        itemInfo->SetName(userName);
        itemInfo->SetDatetime(QDateTime::currentDateTime().toString("MM-dd HH:mm"));
        itemInfo->SetHeadPixmap(MyApp::HeadFullPath(head));
        itemInfo->SetMsgType(static_cast<quint8>(type));
        itemInfo->SetTextColor(jsonObj.value("color").toString());
        if(static_cast<quint8>(type) == Text){
            itemInfo->SetText(msg);
        }else if(static_cast<quint8>(type) == Face){
            itemInfo->SetFace(msg.toInt());
        }else if(static_cast<quint8>(type) == Picture){
            itemInfo->SetText(myHelper::SaveInlineImage(msg));
        }else if(static_cast<quint8>(type) == Audio){
            // 语音消息：同样先落盘成 wav，历史记录才能直接播放
            itemInfo->SetText(myHelper::SaveInlineAudio(msg));
            itemInfo->SetDuration(jsonObj.value("dur").toInt(0));
        }else if(static_cast<quint8>(type) == Files){
            itemInfo->SetText(msg);
            itemInfo->SetFileSizeString(jsonObj.value("size").toString());
        }
        itemInfo->SetMsgType(static_cast<quint8>(type));
        itemInfo->SetOrientation(Left);
        const QString inKey = jsonObj.value("key").toString();
        itemInfo->SetMsgKey(inKey.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : inKey);
        itemInfo->SetTimeStamp(QDateTime::currentMSecsSinceEpoch());
        DatabaseMsg::getInstance()->AddGroupHistoryMsg(userId, groupId, itemInfo);

        // 群消息未打开窗口时算未读
        refreshUnreadBadges();
        refreshConversationList();

        delete itemInfo;
        itemInfo = nullptr;
}




///////////////////////////////////////////////////////////////////////////
/// 会话列表 & 未读角标
///

/**
 * @brief MainWindow::SltConversationClicked
 * 会话列表双击：打开对应的好友/群组聊天窗口
 */
void MainWindow::SltConversationClicked(QQCell *cell)
{
    if (nullptr == cell) {
        return;
    }

    // 会话列表随后会被重建，先把需要的数据拷出来（原 cell 会被释放）
    const bool isGroup = cell->isGroup;
    const int id = cell->id;
    const QString name = cell->name;
    const QString iconPath = cell->iconPath;
    const int status = cell->status;

    // 双击事件处理过程中不能立刻重建列表（会把正在处理事件的控件删掉），延后一轮执行
    QTimer::singleShot(0, this, [=]() {
        QList<ChatWindow *> &windows = isGroup ? m_chatGroupWindows : m_chatFriendWindows;
        for (ChatWindow *window : windows) {
            if (window->getUserId() == id) {
                window->show();
                window->raise();
                DatabaseMsg::getInstance()->markSessionRead(id, isGroup);
                refreshUnreadBadges();
                refreshConversationList();
                return;
            }
        }

        QQCell *chatCell = new QQCell;
        chatCell->id = id;
        chatCell->name = name;
        chatCell->iconPath = iconPath;
        chatCell->status = status;
        chatCell->groupName = tr("最近会话");
        chatCell->type = QQCellType_Child;

        openChatWindow(chatCell, isGroup, true);
    });
}

/**
 * @brief MainWindow::sessionPreview
 * 会话列表里显示的最后一条消息预览
 */
QString MainWindow::sessionPreview(const QJsonObject &obj)
{
    const int type = obj.value("type").toInt();
    const QString content = obj.value("content").toString();

    if (Face == type)    return tr("[动画表情]");
    if (Files == type)   return tr("[文件] ") + content;
    if (Audio == type)   return tr("[语音]");
    if (Picture == type) return tr("[图片]");
    return content;
}

/**
 * @brief MainWindow::refreshConversationList
 * 从消息库重建"会话"页的列表（最近会话，带未读数）
 */
void MainWindow::refreshConversationList()
{
    ui->convationListWidget->clearAllCells();

    QQCell *sessionGroup = new QQCell;
    sessionGroup->groupName = tr("最近会话");
    sessionGroup->name = tr("最近会话");
    sessionGroup->type = QQCellType_GroupEx;
    sessionGroup->isOpen = true;         // 默认展开
    ui->convationListWidget->insertQQCell(sessionGroup);

    const QVector<QJsonObject> sessions = DatabaseMsg::getInstance()->getRecentSessions();
    for (const QJsonObject &obj : sessions) {
        QQCell *cell = new QQCell;
        cell->groupName = sessionGroup->groupName;
        cell->type = QQCellType_Child;
        cell->id = obj.value("id").toInt();
        cell->isGroup = obj.value("isGroup").toBool();
        cell->name = obj.value("name").toString();
        const QString sessionHead = obj.value("head").toString();
        cell->iconPath = MyApp::HeadFullPath(sessionHead);
        if (!cell->isGroup) {
            ensureHeadAvailable(cell->id, sessionHead);
        }
        cell->subTitle = sessionPreview(obj);
        cell->status = OnLine;
        cell->unread = obj.value("unread").toInt();
        ui->convationListWidget->insertQQCell(cell);
    }

    qDebug() << "[会话] 刷新会话列表，共" << sessions.size() << "个会话";
}

/**
 * @brief MainWindow::updateTabBadge
 * 在标签按钮右上角显示/隐藏未读角标
 */
void MainWindow::updateTabBadge(QWidget *btn, int unread)
{
    if (nullptr == btn) {
        return;
    }

    QLabel *badge = m_tabBadges.value(btn, nullptr);
    if (unread <= 0) {
        if (badge) {
            badge->hide();
        }
        return;
    }

    if (nullptr == badge) {
        badge = new QLabel(btn);
        badge->setObjectName("tabUnreadBadge");
        badge->setAlignment(Qt::AlignCenter);
        badge->setStyleSheet("QLabel#tabUnreadBadge{"
                             "background-color:#F5222D;"
                             "color:#FFFFFF;"
                             "border-radius:8px;"
                             "font:bold 10px \"微软雅黑\";}");
        badge->setFixedHeight(16);
        badge->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_tabBadges.insert(btn, badge);
        btn->installEventFilter(this);
    }

    const QString text = unread > 99 ? QString("99+") : QString::number(unread);
    const int width = (unread > 9) ? 24 : 16;
    badge->setText(text);
    badge->setFixedWidth(width);
    badge->move(btn->width() - width - 2, 2);
    badge->raise();
    badge->show();
}

/**
 * @brief MainWindow::refreshUnreadBadges
 * 从消息库统计未读数并刷新标签角标
 */
void MainWindow::refreshUnreadBadges()
{
    DatabaseMsg *db = DatabaseMsg::getInstance();
    const int friendUnread = db->getUnreadTotal(false);
    const int groupUnread  = db->getUnreadTotal(true);

    // 未读标记只放在"会话"页：好友页、群组页不显示任何未读标记
    updateTabBadge(ui->btnFrind, 0);
    updateTabBadge(ui->btnGroup, 0);
    updateTabBadge(ui->btnConversation, friendUnread + groupUnread);

    qDebug() << "[未读] 好友 =" << friendUnread << " 群组 =" << groupUnread
             << " 会话 =" << (friendUnread + groupUnread);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // 右键自己的头像 -> 更换头像
    if (watched == ui->widgetHead && QEvent::ContextMenu == event->type()) {
        QContextMenuEvent *ctxEvent = static_cast<QContextMenuEvent *>(event);
        showHeadMenu(ctxEvent->globalPos());
        return true;
    }

    // 标签按钮尺寸变化时重新摆放角标
    if (QEvent::Resize == event->type()) {
        QWidget *btn = qobject_cast<QWidget *>(watched);
        QLabel *badge = m_tabBadges.value(btn, nullptr);
        if (badge && badge->isVisible()) {
            badge->move(btn->width() - badge->width() - 2, 2);
        }
    }
    return CustomMoveWidget::eventFilter(watched, event);
}

///////////////////////////////////////////////////////////////////////////
/// 申请列表（好友申请 / 加群申请）
///

/**
 * @brief MainWindow::addPendingRequest
 * 收到申请：放进待处理列表，并在 tbtnAddUser 上显示红点
 */
void MainWindow::addPendingRequest(const PendingRequest &req)
{
    // 同一个人、同一类申请只保留一条
    for (const PendingRequest &r : m_pendingRequests) {
        if (r.type == req.type && r.id == req.id && r.groupId == req.groupId) {
            return;
        }
    }

    m_pendingRequests.append(req);
    updateTabBadge(ui->tbtnAddUser, m_pendingRequests.size());
    rebuildRequestPage();

    qDebug() << "[申请] 收到新的申请，待处理" << m_pendingRequests.size() << "条："
             << req.name << (AddGroupRequist == req.type ? req.groupName : QString("好友"));
}

void MainWindow::on_tbtnAddUser_clicked()
{
    showRequestPage();
}

void MainWindow::showRequestPage()
{
    if (nullptr == m_requestPage) return;
    rebuildRequestPage();
    ui->GCStackedWidget->setCurrentWidget(m_requestPage);
}

/**
 * @brief MainWindow::rebuildRequestPage
 * 重建申请列表（每条申请一行：头像 + 名字 + 说明 + 同意/拒绝）
 */
void MainWindow::rebuildRequestPage()
{
    if (nullptr == m_requestList) return;

    m_requestList->clear();

    if (m_pendingRequests.isEmpty()) {
        QListWidgetItem *emptyItem = new QListWidgetItem(tr("暂无新的申请"), m_requestList);
        emptyItem->setTextAlignment(Qt::AlignCenter);
        emptyItem->setFlags(Qt::NoItemFlags);
        emptyItem->setSizeHint(QSize(0, 60));
        return;
    }

    const QString btnStyle =
            "QPushButton{min-width:0px;min-height:0px;padding:0px;font:11px \"微软雅黑\";"
            "border:1px solid #C9C9C9;border-radius:3px;background:#FFFFFF;color:#333333;}"
            "QPushButton:hover{background:#E8F2FF;}";

    for (const PendingRequest &req : m_pendingRequests)
    {
        QWidget *row = new QWidget;
        row->setStyleSheet("background:#FFFFFF;");
        QHBoxLayout *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 6, 8, 6);
        rowLayout->setSpacing(8);

        // 头像
        QLabel *icon = new QLabel(row);
        icon->setFixedSize(36, 36);
        QPixmap headPix(MyApp::m_strHeadPath + "default.png");
        if (!headPix.isNull()) {
            icon->setPixmap(headPix.scaled(36, 36, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        rowLayout->addWidget(icon);

        // 名字 + 申请说明
        QVBoxLayout *textLayout = new QVBoxLayout;
        textLayout->setSpacing(2);
        QLabel *nameLabel = new QLabel(req.name, row);
        nameLabel->setStyleSheet("color:#000000;font:bold 12px \"微软雅黑\";");
        QLabel *descLabel = new QLabel(AddGroupRequist == req.type
                                       ? tr("申请加入群聊'%1'").arg(req.groupName)
                                       : tr("请求添加你为好友"), row);
        descLabel->setStyleSheet("color:#666666;font:11px \"微软雅黑\";");
        textLayout->addWidget(nameLabel);
        textLayout->addWidget(descLabel);
        rowLayout->addLayout(textLayout, 1);

        // 同意 / 拒绝
        QPushButton *btnOk = new QPushButton(tr("同意"), row);
        QPushButton *btnNo = new QPushButton(tr("拒绝"), row);
        btnOk->setStyleSheet(btnStyle);
        btnNo->setStyleSheet(btnStyle);
        btnOk->setFixedSize(46, 22);
        btnNo->setFixedSize(46, 22);
        connect(btnOk, &QPushButton::clicked, this, [this, req]() { handleRequest(req, true); });
        connect(btnNo, &QPushButton::clicked, this, [this, req]() { handleRequest(req, false); });
        rowLayout->addWidget(btnOk);
        rowLayout->addWidget(btnNo);

        QListWidgetItem *item = new QListWidgetItem(m_requestList);
        item->setSizeHint(QSize(0, 56));
        m_requestList->setItemWidget(item, row);
    }
}

/**
 * @brief MainWindow::handleRequest
 * 同意/拒绝一条申请，然后从列表里删掉
 */
void MainWindow::handleRequest(const PendingRequest &req, bool accept)
{
    if (AddFriendRequist == req.type)
    {
        if (accept) {
            DatabaseMsg::getInstance()->AddFriend(req.id, req.name, 1);
            qDebug() << "已同意好友申请" << req.name;
        } else {
            qDebug() << "已拒绝" << req.name << "的好友申请";
        }

        QJsonObject resObj;
        resObj.insert("id", MyApp::m_nId);
        resObj.insert("name", req.name);
        resObj.insert("msg", accept ? QString("accept") : QString("refuse"));
        m_tcpSocket->sendMessage(AddFriendReply, resObj);
    }
    else if (AddGroupRequist == req.type)
    {
        QJsonObject resObj;
        resObj.insert("id", MyApp::m_nId);      // 群主Id
        resObj.insert("to", req.id);            // 申请人
        resObj.insert("groupId", req.groupId);
        resObj.insert("msg", accept ? QString("accept") : QString("refuse"));

        if (accept) {
            DatabaseMsg::getInstance()->addGroupMember(req.id, req.groupId, Member);
        }
        m_tcpSocket->sendMessage(AddGroupReply, QJsonValue(resObj));
    }

    // 从待处理列表移除
    for (int i = m_pendingRequests.size() - 1; i >= 0; --i) {
        const PendingRequest &r = m_pendingRequests.at(i);
        if (r.type == req.type && r.id == req.id && r.groupId == req.groupId) {
            m_pendingRequests.removeAt(i);
        }
    }
    updateTabBadge(ui->tbtnAddUser, m_pendingRequests.size());
    rebuildRequestPage();

    // 不再弹模态提示框：这一行会立即从列表里消失，本身就是反馈
    qDebug() << "[申请] 已处理：" << req.name
             << (accept ? "同意" : "拒绝")
             << " 剩余" << m_pendingRequests.size() << "条";
}

///////////////////////////////////////////////////////////////////////////
/// 会话列表右键：删除会话
///

void MainWindow::onConvMenuSelected(QAction *action)
{
    QQCell *cell = ui->convationListWidget->GetRightClickedCell();
    if (nullptr == cell) return;

    if (!action->text().compare(tr("删除会话")))
    {
        int ret = CMessageBox::Question(this,
                                        QString("删除后该会话的聊天记录将无法恢复，确定删除'%1'吗？").arg(cell->name),
                                        "删除会话");
        if (QDialog::Accepted != ret) return;

        const bool isGroup = cell->isGroup;
        const int sessionId = cell->id;

        // 关掉这个会话的聊天窗口（走正常关闭流程，窗口会把自己从列表里移除）
        QList<ChatWindow *> &windows = isGroup ? m_chatGroupWindows : m_chatFriendWindows;
        for (int i = windows.size() - 1; i >= 0; --i) {
            if (windows.at(i)->getUserId() == sessionId) {
                QMetaObject::invokeMethod(windows.at(i), "on_btnWinClose_clicked");
            }
        }

        // 只删聊天记录，好友/群关系保留
        DatabaseMsg::getInstance()->clearSessionMessages(sessionId, isGroup);

        refreshConversationList();
        refreshUnreadBadges();
        qDebug() << "[会话] 已删除会话" << sessionId << "isGroup=" << isGroup;
    }
}

/**
 * @brief MainWindow::notifyTray
 * 非阻塞提示：优先用托盘气泡；托盘不支持时只写日志，绝不弹模态框
 */
void MainWindow::notifyTray(const QString &title, const QString &content)
{
    qDebug() << "[通知]" << title << "-" << content;

    if (systemTrayIcon && QSystemTrayIcon::supportsMessages()) {
        systemTrayIcon->showMessage(title, content, QSystemTrayIcon::Information, 3000);
    }
}

///////////////////////////////////////////////////////////////////////////
/// 头像
///

/**
 * @brief MainWindow::showHeadMenu
 * 右键自己的头像：更换头像 / 重新获取
 */
void MainWindow::showHeadMenu(const QPoint &globalPos)
{
    QMenu menu(this);
    QAction *changeAction = menu.addAction(tr("更换头像"));
    menu.addSeparator();
    QAction *reloadAction = menu.addAction(tr("重新获取头像"));

    QAction *selected = menu.exec(globalPos);
    if (nullptr == selected) {
        return;
    }
    if (selected == changeAction) {
        changeHead();
    } else if (selected == reloadAction) {
        // 本地文件可能损坏/被别人删掉，删了重新拉
        const QString path = MyApp::HeadFullPath(MyApp::m_strHeadFile);
        if (QFile::exists(path)) {
            QFile::remove(path);
        }
        m_headRequests.remove(MyApp::m_nId);
        requestHead(MyApp::m_nId);
    }
}

/**
 * @brief MainWindow::changeHead
 * 选一张图片 -> 居中裁剪成 96x96 -> 本地立即生效 -> base64 上传到服务器
 */
void MainWindow::changeHead()
{
    const QString filePath = QFileDialog::getOpenFileName(
                this, tr("选择头像"), QDir::homePath(),
                tr("图片文件 (*.png *.jpg *.jpeg *.bmp *.gif)"));
    if (filePath.isEmpty()) {
        return;
    }
    uploadHeadFile(filePath);
}

/**
 * @brief MainWindow::uploadHeadFile
 * 把指定图片处理成 96x96 头像并上传（右键菜单和自动化测试都走这里）
 */
void MainWindow::uploadHeadFile(const QString &filePath)
{
    QImage image(filePath);
    if (image.isNull()) {
        CMessageBox::Warning(this, tr("这张图片无法读取，请换一张"), tr("更换头像"));
        return;
    }

    const int side = qMin(image.width(), image.height());
    if (side <= 0) {
        CMessageBox::Warning(this, tr("图片尺寸异常，请换一张"), tr("更换头像"));
        return;
    }

    // 居中裁剪成正方形再缩放到 96x96，保证所有人看到的尺寸一致
    const QImage headImage = image.copy((image.width() - side) / 2,
                                        (image.height() - side) / 2,
                                        side, side)
            .scaled(96, 96, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_ARGB32);

    QDir().mkpath(MyApp::m_strHeadPath);
    // 文件名带时间戳：别人本地有这个文件名的就说明是最新的，不会拿到旧头像
    const QString headName = QString("head_%1_%2.png")
            .arg(MyApp::m_nId)
            .arg(QDateTime::currentDateTime().toString("yyyyMMddHHmmsszzz"));
    const QString localPath = MyApp::m_strHeadPath + headName;
    if (!headImage.save(localPath, "PNG")) {
        CMessageBox::Warning(this, tr("头像保存失败，请检查磁盘权限"), tr("更换头像"));
        return;
    }

    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        CMessageBox::Warning(this, tr("头像读取失败"), tr("更换头像"));
        return;
    }
    const QByteArray raw = file.readAll();
    file.close();

    QJsonObject jsonObj;
    jsonObj.insert("id", MyApp::m_nId);
    jsonObj.insert("head", headName);
    jsonObj.insert("data", QString::fromLatin1(raw.toBase64()));
    m_tcpSocket->sendMessage(UpdateHeadPic, jsonObj);

    // 本地先立即生效，服务器确认后会用同一个文件名再走一遍
    MyApp::m_strHeadFile = headName;
    setHead(localPath);

    qDebug() << "[头像] 已上传头像：" << headName << "大小" << raw.size() << "字节";
}

/**
 * @brief MainWindow::saveHeadImage
 * 把服务器下发的 base64 头像写成本地文件，返回保存路径
 */
QString MainWindow::saveHeadImage(const QString &head, const QString &base64)
{
    if (head.isEmpty() || base64.isEmpty()) {
        qDebug() << "[头像] 下发数据为空，忽略";
        return QString();
    }

    const QByteArray raw = QByteArray::fromBase64(base64.toLatin1());
    if (raw.isEmpty()) {
        qWarning() << "[头像] base64 解码失败：" << head;
        return QString();
    }

    QDir().mkpath(MyApp::m_strHeadPath);
    const QString path = MyApp::HeadFullPath(head);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "[头像] 写入失败：" << path << file.errorString();
        return QString();
    }
    file.write(raw);
    file.close();

    return path;
}

/**
 * @brief MainWindow::ensureHeadAvailable
 * 本地没有这个头像文件就向服务器拉一次（同一时间只拉一次）
 */
void MainWindow::ensureHeadAvailable(int userId, const QString &head)
{
    if (userId <= 0) {
        return;
    }

    const QString name = QFileInfo(head).fileName();
    if (name.isEmpty()) {
        return;
    }
    // 默认头像客户端自带，不用向服务器要
    if (0 == name.compare("default.png", Qt::CaseInsensitive)
            || 0 == name.compare("default_group.png", Qt::CaseInsensitive)) {
        return;
    }
    if (QFile::exists(MyApp::HeadFullPath(head))) {
        return;
    }

    requestHead(userId);
}

void MainWindow::requestHead(int userId)
{
    if (userId <= 0 || m_headRequests.contains(userId)) {
        return;
    }
    m_headRequests.insert(userId);

    QJsonObject jsonObj;
    jsonObj.insert("id", userId);
    m_tcpSocket->sendMessage(GetUserHead, jsonObj);

    // 服务器没回应时放开标记，避免以后永远不再请求
    QTimer::singleShot(5000, this, [this, userId]() {
        m_headRequests.remove(userId);
    });
}

/**
 * @brief MainWindow::refreshHeadUi
 * 某个人的头像变了：刷新好友行、会话列表和已打开的聊天窗口
 */
void MainWindow::refreshHeadUi(int userId)
{
    if (userId <= 0) {
        return;
    }

    const QString headPath = MyApp::HeadFullPath(DatabaseMsg::getInstance()->getFriendHead(userId));

    bool changed = false;
    const QList<QQCell *> groups = ui->frindListWidget->getCells();
    for (QQCell *group : groups) {
        for (QQCell *child : group->childs) {
            if (QQCellType_Child == child->type && child->id == userId) {
                child->iconPath = headPath;
                changed = true;
            }
        }
    }
    if (changed) {
        ui->frindListWidget->upload();
    }

    // 会话列表整表重建，头像从库里取
    refreshConversationList();

    // 聊天气泡每次重绘都会按路径重新加载图片，重绘一下即可换过来
    for (ChatWindow *window : m_chatFriendWindows) {
        if (window->getUserId() == userId) {
            window->update();
        }
    }
    for (ChatWindow *window : m_chatGroupWindows) {
        window->update();
    }
}

// ==================================================================
// 端到端加密（客户端侧）
// ------------------------------------------------------------------
// 发送：明文 --AES-256-GCM--> 密文，AES 密钥用每个收件人 RSA 公钥包裹
// 接收：用本机私钥解开自己的那份 AES 密钥 -> 解密正文
// 服务器全程只转发、存储密文
// ==================================================================

/**
 * @brief 登录后立即在本地加载/生成本机密钥对
 * 这一步完全不依赖服务器：私钥是本机生成的，公钥随后才需要上报。
 * 以前把密钥初始化挂在"收到管理员公钥"上，导致客户端在收到管理员公钥之前
 * 收到的消息（尤其是登录瞬间下发的离线积压消息）一律解不开。
 */
void MainWindow::initLocalCrypto()
{
    KeyStore *ks = KeyStore::instance();

    const QString pub = ks->ensureLocalKeys(MyApp::m_strUserName, MyApp::m_nId);
    if (pub.isEmpty()) {
        qWarning() << "[加密] 本机密钥初始化失败：" << CryptoUtil::lastError();
        notifyTray("加密初始化失败", "本机密钥生成失败，消息将无法加密收发");
        return;
    }

    // 自己的公钥也放进缓存，密文包里要留一份给自己（用于本机回看历史记录）
    ks->cachePubKey(MyApp::m_nId, pub);
    m_bCryptoReady = true;

    qInfo() << "[加密] 本机密钥就绪，公钥长度" << pub.size();

    // 把公钥登记到服务器，别人才能给它发加密消息
    uploadKeys();

    // 密钥就绪，把之前暂存的密文消息补上
    sltFlushPendingIncoming();
}

/**
 * @brief 把本机公钥登记到服务器，供他人加密时取用
 * 只上传公钥（公开信息）；私钥永远不出本机，服务器不存在任何私钥副本。
 */
void MainWindow::uploadKeys()
{
    KeyStore *ks = KeyStore::instance();
    if (!ks->isReady()) return;

    QJsonObject up;
    up.insert("pub", ks->localPubKey());
    m_tcpSocket->sendMessage(UploadKeys, QJsonValue(up));

    qInfo() << "[加密] 已上报本机公钥（仅公钥，不含任何私钥材料）";
}

/**
 * @brief MainWindow::handleRecallMsg
 * 对端撤回了一条消息：优先交给已打开的聊天窗口处理（界面 + 库一起清），
 * 窗口没开就直接删库，等下次打开聊天记录时自然看不到。
 */
void MainWindow::handleRecallMsg(const QJsonValue &dataVal)
{
    if (!dataVal.isObject()) return;

    const QJsonObject obj = dataVal.toObject();
    const QString key = obj.value("key").toString();
    if (key.isEmpty()) return;

    const bool isGroup = (obj.value("group").toInt() != 0);
    if (isGroup)
    {
        const int groupId = obj.value("gid").toInt(obj.value("id").toInt());
        for (ChatWindow *window : m_chatGroupWindows)
        {
            if (window->getUserId() == groupId)
            {
                window->removeMsgByKey(key, true);
                refreshConversationList();
                return;
            }
        }
    }
    else
    {
        const int fromId = obj.value("id").toInt();
        for (ChatWindow *window : m_chatFriendWindows)
        {
            if (window->getUserId() == fromId)
            {
                window->removeMsgByKey(key, true);
                refreshConversationList();
                return;
            }
        }
    }

    // 聊天窗口没打开：库里删掉即可
    DatabaseMsg::getInstance()->deleteMsgByKey(key);
    refreshConversationList();
    qDebug() << "[撤回] 窗口未打开，已直接删除" << key;
}

/**
 * @brief MainWindow::handleSendAck
 * 服务端回执：我发的某条消息已被受理，把对应气泡的状态从"发送中"改成已送达。
 * 回执只带 key，不带会话信息，所以两个窗口列表都扫一遍（key 全局唯一，命中即止）。
 */
void MainWindow::handleSendAck(const QJsonValue &dataVal)
{
    if (!dataVal.isObject()) return;

    const QJsonObject obj = dataVal.toObject();
    const QString key = obj.value("key").toString();
    if (key.isEmpty()) return;

    for (ChatWindow *window : m_chatFriendWindows)
    {
        if (window && window->hasMsgKey(key))
        {
            window->onSendAck(key);
            return;
        }
    }
    for (ChatWindow *window : m_chatGroupWindows)
    {
        if (window && window->hasMsgKey(key))
        {
            window->onSendAck(key);
            return;
        }
    }

    qDebug() << "[发送] 收到未知回执" << key;
}

/**
 * @brief MainWindow::failAllPendingSends
 * 链路断开：所有还在等回执的消息一律标红，用户重连后可以右键重发。
 */
void MainWindow::failAllPendingSends(const QString &reason)
{
    for (ChatWindow *window : m_chatFriendWindows)
    {
        if (window) window->failPendingSends(reason);
    }
    for (ChatWindow *window : m_chatGroupWindows)
    {
        if (window) window->failPendingSends(reason);
    }
}

/**
 * @brief 收到消息的统一入口：先解密，再交给既有的显示/入库流程
 * 关键点：解密与"聊天窗口是否打开"完全无关，消息一到就解。
 */
void MainWindow::deliverIncoming(quint8 type, const QJsonValue &dataVal)
{
    int result = 0;
    const QJsonValue decrypted = decryptIfNeeded(dataVal, &result);

    // 本机密钥还没就绪：先攒着，不显示、更不落库（否则密文会被错误文本覆盖掉）
    if (1 == result) {
        PendingIncoming pending;
        pending.type = type;
        pending.data = dataVal;
        m_pendingIncoming.append(pending);
        qInfo() << "[加密] 本机密钥未就绪，暂存待解密消息，当前队列" << m_pendingIncoming.size();

        // 兜底：正常情况密钥在构造函数里就绪，这里只是防止极端情况下消息被无限期压着
        QTimer::singleShot(8000, this, [this]() {
            if (!KeyStore::instance()->isReady()) initLocalCrypto();
            sltFlushPendingIncoming();
        });
        return;
    }

    if (SendGroupMsg == type) {
        sendGroupMsg(decrypted);
    } else {
        receiveMessage(decrypted);
    }
}

/**
 * @brief 把密钥就绪前暂存的密文消息按原顺序重放
 */
void MainWindow::sltFlushPendingIncoming()
{
    if (m_pendingIncoming.isEmpty()) return;
    if (!KeyStore::instance()->isReady()) return;   // 还是没密钥，继续等

    const QList<PendingIncoming> queue = m_pendingIncoming;
    m_pendingIncoming.clear();
    qInfo() << "[加密] 本机密钥就绪，开始补解密" << queue.size() << "条暂存消息";

    for (const PendingIncoming &pending : queue) {
        int result = 0;
        const QJsonValue decrypted = decryptIfNeeded(pending.data, &result);
        if (1 == result) {
            // 仍然不行（比如密钥刚被别人覆盖），放回队列等下次
            m_pendingIncoming.append(pending);
            continue;
        }
        if (SendGroupMsg == pending.type) {
            sendGroupMsg(decrypted);
        } else {
            receiveMessage(decrypted);
        }
    }
}

/**
 * @brief 收到消息时，如果是密文包就用本机私钥解密
 * 解出来的明文会替换 msg 字段并去掉 enc 标记，后续流程与普通消息完全一致
 * @param result 0=正常（明文或解密成功） 1=本机密钥未就绪，需等待重试 2=确实解不开
 */
QJsonValue MainWindow::decryptIfNeeded(const QJsonValue &dataVal, int *result)
{
    if (result) *result = 0;
    if (!dataVal.isObject()) return dataVal;

    QJsonObject obj = dataVal.toObject();
    if (obj.value("enc").toInt() == 0) return dataVal;

    KeyStore *ks = KeyStore::instance();

    // 本机密钥还没就绪：这不是"解不开"，而是"还没准备好"，交给调用方暂存重试
    if (!ks->isReady()) {
        if (result) *result = 1;
        return dataVal;
    }

    const QString packet = obj.value("msg").toString();
    QString plain, err;
    if (ks->decrypt(packet, &plain, &err)) {
        obj.insert("msg", plain);
        obj.remove("enc");
        return QJsonValue(obj);
    }

    // 到这里才是真的解不开：多半是这条密文里没有发给本人的那份密钥
    qWarning() << "[加密] 消息解密失败：" << err;
    if (result) *result = 2;
    obj.insert("msg", QString("[这条消息无法解密：不是发给本机的密文]"));
    obj.remove("enc");
    return QJsonValue(obj);
}

/**
 * @brief 向服务器索取对端公钥（重复请求会被合并）
 */
void MainWindow::requestPubKeys(const QList<int> &ids)
{
    QList<int> need;
    foreach (int id, ids) {
        if (id <= 0 || id == MyApp::m_nId) continue;
        if (m_pendingKeyRequests.contains(id)) continue;
        if (!KeyStore::instance()->missingKeys(QList<int>() << id).isEmpty())
            need.append(id);
    }
    if (need.isEmpty()) return;

    foreach (int id, need) m_pendingKeyRequests.insert(id);

    QJsonArray arr;
    foreach (int id, need) arr.append(id);

    QJsonObject req;
    req.insert("ids", arr);
    m_tcpSocket->sendMessage(GetPubKey, QJsonValue(req));
    qDebug() << "[加密] 请求公钥：" << need;
}

/**
 * @brief 打开会话窗口时预热密钥，避免第一次发消息还要等
 */
void MainWindow::prefetchChatKeys(ChatWindow *window, bool isGroup, int peerId)
{
    Q_UNUSED(window)
    KeyStore *ks = KeyStore::instance();
    if (!ks->isReady()) return;

    QList<int> ids;
    if (isGroup) {
        ids = DatabaseMsg::getInstance()->getGroupMemberIds(peerId);
    } else if (peerId > 0) {
        ids << peerId;
    }
    ids.removeAll(MyApp::m_nId);
    if (ids.isEmpty()) return;

    // 把还缺的公钥一次性要过来
    QList<int> need;
    foreach (int id, ids) {
        if (!ks->hasPubKey(id)) need << id;
    }
    if (!need.isEmpty()) requestPubKeys(need);
}

/**
 * @brief 把待发消息的 msg 换成密文包
 * @param reason 0=加密成功 1=正在等对方公钥 2=加密失败
 */
bool MainWindow::encryptOutgoing(QJsonValue &dataVal, quint8 type, int *reason)
{
    if (reason) *reason = 0;
    if (!dataVal.isObject()) return true;   // 非对象（理论上不会出现）直接放行

    QJsonObject obj = dataVal.toObject();
    // 正文一律加密：文字、图片（b64）、语音（b64）、文件元信息、表情下标都走同一条密文链路。
    // 服务器只接触密文包，不掌握任何接收者的私钥，因此无法还原其中任何一类内容。

    KeyStore *ks = KeyStore::instance();
    if (!ks->isReady()) {
        if (reason) *reason = 1;
        return false;
    }

    QList<int> recipients;
    if (type == SendGroupMsg) {
        // 群文件消息历史上把发送者放在 id、群号放在 to，这里跟着服务端的口径做兼容
        int gid = obj.value("id").toInt();
        if (obj.value("type").toInt() == Files) gid = obj.value("to").toInt();
        recipients = DatabaseMsg::getInstance()->getGroupMemberIds(gid);
    } else {
        recipients << obj.value("id").toInt();
    }
    recipients.removeAll(MyApp::m_nId);       // 自己那份由 KeyStore 自动补
    if (recipients.isEmpty()) {
        if (reason) *reason = 2;
        return false;
    }

    const QList<int> miss = ks->missingKeys(recipients);
    if (!miss.isEmpty()) {
        requestPubKeys(miss);
        if (reason) *reason = 1;
        return false;
    }

    QString err;
    const QString packet = ks->encryptFor(obj.value("msg").toString(), recipients,
                                          MyApp::m_nId, &err);
    if (packet.isEmpty()) {
        qWarning() << "[加密] 加密消息失败：" << err;
        if (reason) *reason = 2;
        return false;
    }

    obj.insert("msg", packet);
    obj.insert("enc", 1);
    dataVal = QJsonValue(obj);
    return true;
}

/**
 * @brief 聊天窗口发消息的入口：先加密再交给 socket
 */
void MainWindow::sltChatSendMessage(const quint8 &type, const QJsonValue &dataVal)
{
    // 聊天类消息统一走加密：文字 / 图片 / 语音 / 表情 / 群文件元信息
    if (type != SendMsg && type != SendGroupMsg && type != SendFace) {
        m_tcpSocket->sendMessage(type, dataVal);
        return;
    }

    int reason = 0;
    QJsonValue out = dataVal;
    if (encryptOutgoing(out, type, &reason)) {
        m_tcpSocket->sendMessage(type, out);
        return;
    }

    if (reason == 1) {
        // 对方公钥还没拿到：先排队，PubKeyData 回来后再补发
        PendingMsg pm;
        pm.type = type;
        pm.data = dataVal;
        m_pendingSendMsgs.append(pm);
        qDebug() << "[加密] 公钥未就绪，消息已排队，当前待发" << m_pendingSendMsgs.size() << "条";
        notifyTray("正在同步加密公钥", "对方公钥获取中，消息将在就绪后自动发出");
        return;
    }

    CMessageBox::Warning(this, "无法加密这条消息，已取消发送。\n请确认对方已登录过本客户端。", "端到端加密");
}

/**
 * @brief 公钥到齐 / 密钥上传完成后，把排队的消息发出去
 */
void MainWindow::sltFlushPendingMessages()
{
    if (m_pendingSendMsgs.isEmpty()) return;
    if (!KeyStore::instance()->isReady()) return;

    QList<PendingMsg> pending = m_pendingSendMsgs;
    m_pendingSendMsgs.clear();

    for (int i = 0; i < pending.size(); ++i) {
        PendingMsg pm = pending.at(i);
        int reason = 0;
        QJsonValue out = pm.data;
        if (encryptOutgoing(out, pm.type, &reason)) {
            m_tcpSocket->sendMessage(pm.type, out);
            continue;
        }
        if (reason == 1 && pm.retry < 6) {
            ++pm.retry;
            m_pendingSendMsgs.append(pm);   // 再等一轮
        } else {
            notifyTray("消息未发送", "对方公钥不可用，消息已取消");
        }
    }
}

/**
 * @brief 定时重试：对方可能稍后才登录并上传公钥
 */
void MainWindow::sltRetryPendingMessages()
{
    if (m_pendingSendMsgs.isEmpty()) return;
    m_pendingKeyRequests.clear();          // 允许重新向服务器索取公钥
    sltFlushPendingMessages();
}

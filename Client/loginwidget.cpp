#include "loginwidget.h"
#include "ui_loginwidget.h"
#include "mainwindow.h"
#include "netdb/mysocket.h"
#include "netdb/databasemsg.h"
#include "netdb/keystore.h"
#include "comapi/myapp.h"
#include "comapi/unit.h"
#include "comapi/cryptoutil.h"
#include <comapi/global.h>

#include <QMessageBox>
#include <QEvent>
#include <QResizeEvent>
#include <QLabel>

#include <comapi/myapp.h>

LoginWidget::LoginWidget(QWidget *parent) :
    CustomMoveWidget(parent),
    ui(new Ui::LoginWidget)
{
    ui->setupUi(this);
    setQss(QString(":/qss/resource/qss/default.css"));

    // 顶部背景图：按控件尺寸等比缩放铺满（超出部分居中裁掉），
    // 不再让 3968×1024 的原图按像素尺寸把登录窗口撑变形。
    m_bgPixmap = QPixmap(":/resource/background/logo.jpg");
    ui->labelWeclcom->setAlignment(Qt::AlignCenter);
    ui->labelWeclcom->setScaledContents(false);
    ui->labelWeclcom->installEventFilter(this);
    updateBanner();

    // 登录框同样是无边框窗口，补上最小化样式位，任务栏图标才能点
    setWindowFlags(Qt::FramelessWindowHint
                   | Qt::WindowMinimizeButtonHint
                   | Qt::WindowSystemMenuHint);
    // 数据目录必须锚定在 exe 所在目录，不能用 "."：
    // 从快捷方式/命令行/别的目录启动时工作目录不一定是程序目录，
    // 那样 Data 会被建到别处（聊天记录、密钥全跑到意外位置）。
    MyApp::InitApp(QCoreApplication::applicationDirPath());
    m_tcpSocket = new MySocket(this);
    connect(m_tcpSocket,&MySocket::signalStatus,this,&LoginWidget::sltStatus);
    connect(m_tcpSocket,&MySocket::signalConnectSuccess,this,&LoginWidget::sltConnectSuccess);
    connect(m_tcpSocket,&MySocket::signalConnectFailed,this,&LoginWidget::sltConnectFailed);

    // 配置文件里的服务器地址优先，连不上/改地址后都由配置页"确定"触发重连
    loadCfgToUi();

    // "记住密码"决定口令是否落盘，"自动登录"决定下次启动要不要自动提交。
    // 两者是包含关系：自动登录必须能拿到口令，所以勾自动登录时强制勾上记住密码。
    QSettings settings(MyApp::m_strIniFile, QSettings::IniFormat);
    const bool autoLogin = settings.value("UserConfig/autoLogin", false).toBool();
    const bool remember  = autoLogin || settings.value("UserConfig/rememberPwd", true).toBool();

    ui->checkBoxPasswd->setChecked(remember);
    ui->checkBoxAutoLogin->setChecked(autoLogin);

    // 勾"自动登录" → 顺带勾上"记住密码"；取消"记住密码" → "自动登录"同时失效
    connect(ui->checkBoxAutoLogin, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) ui->checkBoxPasswd->setChecked(true);
    });
    connect(ui->checkBoxPasswd, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked) ui->checkBoxAutoLogin->setChecked(false);
    });

    if (!MyApp::m_strUserName.isEmpty()) {
        ui->lineEditUser->setText(MyApp::m_strUserName);
        ui->lineEditUser->setCursorPosition(ui->lineEditUser->text().length());
    }
    ui->lineEditPasswd->clear();
    // 只有"记住密码"且本地确实有口令时才回填；自动登录再额外要求勾了自动登录
    if (remember && !MyApp::m_strUserName.isEmpty() && !MyApp::m_strPassword.isEmpty()) {
        ui->lineEditPasswd->setText(MyApp::m_strPassword);
        m_bAutoLogin = autoLogin;
    }
    // 光标直接落在密码框，省得再点一下
    ui->lineEditPasswd->setFocus();

    ui->labelWinTitle->setText(QString("正在连接服务器(%1:%2)...")
                               .arg(MyApp::m_strHostAddr).arg(MyApp::m_nMsgPort));
    m_tcpSocket->connectToHost(MyApp::m_strHostAddr,MyApp::m_nMsgPort);
}

/**
 * @brief LoginWidget::updateBanner
 * 顶部背景图自适应：按控件的当前尺寸等比缩放，铺满整个横幅区域。
 * 用 KeepAspectRatioByExpanding 而不是 Stretch：图片不会被拉扁变形，
 * 多出来的部分由 QLabel 的居中对齐自然裁掉。
 * 缩放结果按尺寸缓存，窗口尺寸没变时不重复做平滑缩放（大图缩放很费 CPU）。
 */
void LoginWidget::updateBanner()
{
    if (!ui || m_bgPixmap.isNull() || !ui->labelWeclcom) return;

    QLabel *label = ui->labelWeclcom;
    const QSize target = label->size();
    if (target.width() <= 0 || target.height() <= 0) return;

    // 高分屏（例如 200% 缩放）要按物理像素缩放，否则原图缩小后又被按逻辑像素
    // 拉伸放大，边缘会发虚；setDevicePixelRatio 让 QLabel 按逻辑尺寸绘制。
    const qreal dpr = label->devicePixelRatioF() > 0 ? label->devicePixelRatioF() : 1.0;
    const QSize pixelSize(qRound(target.width() * dpr), qRound(target.height() * dpr));

    if (m_bgScaledSize != pixelSize) {
        m_bgScaled = m_bgPixmap.scaled(pixelSize, Qt::KeepAspectRatioByExpanding,
                                       Qt::SmoothTransformation);
        m_bgScaled.setDevicePixelRatio(dpr);
        m_bgScaledSize = pixelSize;
    }

    // 同一张图就不要重复 setPixmap 了，否则会引发无谓的重新布局
    const QPixmap *current = label->pixmap();
    if (current && current->cacheKey() == m_bgScaled.cacheKey()) return;

    label->setPixmap(m_bgScaled);
}

/**
 * @brief LoginWidget::eventFilter
 * 背景图控件的尺寸一变（首次显示、窗口缩放）就重新缩放图片
 */
bool LoginWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->labelWeclcom && event->type() == QEvent::Resize) {
        updateBanner();
    }
    return CustomMoveWidget::eventFilter(watched, event);
}

void LoginWidget::loadCfgToUi()
{    ui->lineEditHostAddr->setText(MyApp::m_strHostAddr);
    ui->lineEditHostMsgPort->setText(QString::number(MyApp::m_nMsgPort));
    ui->lineEditHostFilePort->setText(QString::number(MyApp::m_nFilePort));
}

void LoginWidget::sltConnectSuccess()
{
    if (!m_tcpSocket->isConnected()) {
        return;
    }
    ui->labelWinTitle->setText("已连接服务器");
    qDebug() << "登录界面已连接服务器";

    // 自动登录：只在第一次连接成功时触发，失败后由用户手动点登录
    if (m_bAutoLogin) {
        m_bAutoLogin = false;
        if (!ui->lineEditUser->text().isEmpty() && !ui->lineEditPasswd->text().isEmpty()) {
            qDebug() << "[自动登录]" << ui->lineEditUser->text();
            on_btnLogin_clicked();
        }
    }
}

void LoginWidget::sltConnectFailed(const QString &errorString)
{
    // 只有停留在登录页时才提示，已经登录成功后主窗口自己有断线逻辑。
    // MySocket 会按指数退避自动重试，所以这里只说明"正在重试"，不给用户压力。
    if (m_tcpSocket->autoReconnect()) {
        ui->labelWinTitle->setText(QString("连接服务器失败：%1（正在自动重试…）")
                                   .arg(errorString));
    } else {
        ui->labelWinTitle->setText(QString("连接服务器失败：%1").arg(errorString));
    }
    qDebug() << "登录界面连接服务器失败:" << errorString;
}

LoginWidget::~LoginWidget()
{
    delete ui;
}

/**
 * @brief LoginWidget::saveLoginCfg
 * 登录成功后保存账号与口令："记住密码"决定口令是否落盘，"自动登录"决定下次是否自动提交
 */
void LoginWidget::saveLoginCfg()
{
    const bool autoLogin = ui->checkBoxAutoLogin->isChecked();
    // 自动登录必然需要口令，所以它也算"记住密码"
    const bool remember  = autoLogin || ui->checkBoxPasswd->isChecked();

    // 先把开关落到内存变量，SavePassword 据此决定"写密文"还是"删掉这一项"
    MyApp::m_bRememberPwd = remember;
    MyApp::SaveConfig();                 // 账号 / 服务器 / rememberPwd
    MyApp::SavePassword(MyApp::m_strPassword);

    QSettings settings(MyApp::m_strIniFile, QSettings::IniFormat);
    settings.setValue("UserConfig/autoLogin", autoLogin);
    settings.sync();
}

void LoginWidget::loginSuccess(const QJsonValue &dataVal)
{
    if(!dataVal.isObject())
    {
        qDebug() << "登录功能出错";
        return;
    }
    QJsonObject jsonObj = dataVal.toObject();
    MyApp::m_nId = jsonObj.value("id").toInt();
    MyApp::m_strHeadFile = jsonObj.value("head").toString();

    disconnect(m_tcpSocket,&MySocket::signalStatus,this,&LoginWidget::sltStatus);
    MyApp::m_strUserName = ui->lineEditUser->text();
    MyApp::m_strPassword = ui->lineEditPasswd->text();
    // 记住账号（勾了自动登录才会把密码留在本地）
    saveLoginCfg();
    //打开该用户的数据库
    qDebug() << "MyApp::m_nId" << MyApp::m_nId;
    DatabaseMsg::getInstance()->OpenUserDatabase(QString("%1user%2.db").arg(MyApp::m_strDatabasePath).arg(MyApp::m_nId));
    DatabaseMsg::getInstance()->OpenMessageDatabase(QString("%1msg%2.db").arg(MyApp::m_strDatabasePath).arg(MyApp::m_nId));

    this->hide();
    MainWindow *mainwdo = new MainWindow(m_tcpSocket);
    mainwdo->show();
}

void LoginWidget::sltStatus(const quint8 &status, const QJsonValue &dataVal)
{
    switch (status) {
    case LoginSuccess:
    {
        loginSuccess(dataVal);
        break;
    }
    case LoginRepeat:
    {
        QMessageBox::warning(this,"登录","该账号已在线");
        break;
    }
    case LoginPasswdError:
    {
        // 服务端为了不泄露"账号是否存在"，统一回 noneUser；
        // 但另两种原因必须分开讲，否则用户永远以为是口令打错了：
        //   keyMismatch —— 本机私钥与账号登记的公钥对不上（换机、重装、多份运行目录各生成了一套密钥）
        //   locked      —— 连续失败触发了 5 分钟锁定
        const QString why = dataVal.isObject()
                ? dataVal.toObject().value("msg").toString() : QString();
        if (why == QLatin1String("keyMismatch")) {
            QMessageBox::warning(this, "登录",
                "本机身份密钥与账号记录不一致。\n\n"
                "若确实是本人在登录，请联系管理员执行：\n"
                "UPDATE USERINFO SET pub_key='' WHERE name='<账号>';");
        } else if (why == QLatin1String("locked")) {
            QMessageBox::warning(this, "登录", "失败次数过多，请 5 分钟后再试。");
        } else {
            QMessageBox::warning(this, "登录", "用户名或密码错误");
        }
        break;
    }
    case RegisterOk:
    {
        QMessageBox::information(this,"注册","新用户注册成功");
        break;
    }
    case RegisterFailed:
    {
        QMessageBox::critical(this,"注册","注册失败");
        break;
    }

    }
}

void LoginWidget::on_btnLogin_clicked()     //登录
{
    if (ui->lineEditUser->text().trimmed().isEmpty()) {
        CMessageBox::Warning(this, "请输入用户名", "登录");
        ui->lineEditUser->setFocus();
        return;
    }
    if (ui->lineEditPasswd->text().isEmpty()) {
        CMessageBox::Warning(this, "请输入密码", "登录");
        ui->lineEditPasswd->setFocus();
        return;
    }

    // 没连上服务器就直接发，sendMessage 里会默默失败，用户只会觉得"点了没反应"
    if (m_tcpSocket->state() != QAbstractSocket::ConnectedState) {
        if (m_tcpSocket->state() == QAbstractSocket::ConnectingState ||
            m_tcpSocket->state() == QAbstractSocket::HostLookupState) {
            QMessageBox::information(this, "登录", "正在连接服务器，请稍候...");
        } else {
            int ret = QMessageBox::question(this, "登录",
                                            QString("尚未连接到服务器（%1:%2），是否检查服务器配置？")
                                            .arg(MyApp::m_strHostAddr).arg(MyApp::m_nMsgPort),
                                            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (ret == QMessageBox::Yes) {
                on_btnWinMenu_clicked();
            }
        }
        return;
    }

    // 登录前就把本机身份密钥准备好：挑战-应答要提交身份私钥签名。
    // 密钥只在本机生成，私钥永不出本机。
    const QString name = ui->lineEditUser->text().trimmed();
    if (KeyStore::instance()->ensureLocalKeys(name, 0).isEmpty()) {
        qWarning() << "[加密] 本机身份密钥初始化失败：" << CryptoUtil::lastError();
    }

    // 协议 v2：sendLogin 只发用户名，口令留在本机参与挑战-应答，不上网
    m_tcpSocket->sendLogin(name, ui->lineEditPasswd->text());
}

void LoginWidget::on_btnReister_clicked()   //注册
{
    QString name = ui->lineEditUser->text();
    QString passwd = ui->lineEditPasswd->text();
    if(name.isEmpty() || passwd.isEmpty()){
        QMessageBox::warning(this,"注册","用户名或密码不能为空");
        return;
    }
    if (m_tcpSocket->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::warning(this, "注册", "尚未连接到服务器，请先配置并连接服务器");
        return;
    }
    QJsonObject json;
    json.insert("name",name);
    json.insert("passwd",passwd);
    m_tcpSocket->sendMessage(Register, json);
}

void LoginWidget::on_btnWinMenu_clicked()
{
    // 每次打开配置页都重新填入当前生效的值，避免上次没保存的内容残留
    loadCfgToUi();
    ui->stackedWidget->setCurrentIndex(1);
    ui->lineEditHostAddr->setFocus();
    ui->lineEditHostAddr->selectAll();
}

void LoginWidget::on_btnCancel_clicked()
{
    // 取消：丢弃输入框里的修改，回到登录页
    loadCfgToUi();
    ui->stackedWidget->setCurrentIndex(0);
}

/**
 * @brief LoginWidget::on_btnSaveCfg_clicked
 * 保存服务器配置并立即重连
 */
void LoginWidget::on_btnSaveCfg_clicked()
{
    const QString ip = ui->lineEditHostAddr->text().trimmed();
    if (!myHelper::IsIP(ip)) {
        QMessageBox::warning(this, "服务器设置", "服务器地址格式不正确，例如：192.168.1.100");
        ui->lineEditHostAddr->setFocus();
        ui->lineEditHostAddr->selectAll();
        return;
    }

    bool ok = false;
    const int msgPort = ui->lineEditHostMsgPort->text().trimmed().toInt(&ok);
    if (!ok || msgPort <= 0 || msgPort > 65535) {
        QMessageBox::warning(this, "服务器设置", "消息服务器端口必须是 1~65535 之间的数字");
        ui->lineEditHostMsgPort->setFocus();
        ui->lineEditHostMsgPort->selectAll();
        return;
    }

    const int filePort = ui->lineEditHostFilePort->text().trimmed().toInt(&ok);
    if (!ok || filePort <= 0 || filePort > 65535) {
        QMessageBox::warning(this, "服务器设置", "文件服务器端口必须是 1~65535 之间的数字");
        ui->lineEditHostFilePort->setFocus();
        ui->lineEditHostFilePort->selectAll();
        return;
    }

    const bool changed = (ip != MyApp::m_strHostAddr)
            || (msgPort != MyApp::m_nMsgPort)
            || (filePort != MyApp::m_nFilePort);

    // 更新内存中的配置并落盘到 Data/Conf/config.ini
    MyApp::m_strHostAddr = ip;
    MyApp::m_nMsgPort    = static_cast<quint16>(msgPort);
    MyApp::m_nFilePort   = static_cast<quint16>(filePort);
    MyApp::SaveConfig();

    // 回到登录页
    ui->stackedWidget->setCurrentIndex(0);

    if (changed || m_tcpSocket->state() != QAbstractSocket::ConnectedState) {
        // 地址/端口变了（或本来就没连上）→ 断开旧连接，用新地址重连
        ui->labelWinTitle->setText(QString("正在连接服务器(%1:%2)...").arg(ip).arg(msgPort));
        m_tcpSocket->connectToHost(ip, static_cast<quint16>(msgPort));
    } else {
        ui->labelWinTitle->setText("已连接服务器");
    }

    qDebug() << "服务器配置已保存:" << ip << msgPort << filePort;
}

void LoginWidget::on_btnWinClose_2_clicked()
{
    on_btnCancel_clicked();     // 配置页的关闭按钮等同于取消
}

void LoginWidget::on_btnWinMin_2_clicked()
{
    showMinimized();
}

void LoginWidget::on_btnWinClose_clicked()
{
    close();        // 登录页关闭 → 退出程序
}

void LoginWidget::on_btnWinMin_clicked()
{
    showMinimized();
}


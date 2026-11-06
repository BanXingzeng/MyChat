#include "chatwindow.h"
#include "ui_chatwindow.h"
#include <QPalette>
#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include <QToolTip>
#include <QDateTime>
#include <QUuid>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QIODevice>
#include <qthread.h>
#include "comapi/unit.h"
#include <basewidget/iteminfo.h>
#include <comapi/global.h>
#include <comapi/myapp.h>
#include <QKeyEvent>
#include <QMenu>
#include <QSettings>
#include <QFontDialog>
#include <QColorDialog>
#include <QStandardPaths>
#include <QBuffer>
#include <QImageReader>
#include <QScreen>
#include <QMessageBox>
#include <QDir>
#include <QToolButton>
#include <netdb/filesocket.h>
#include <netdb/keystore.h>
#include <comapi/cryptoutil.h>
#include "netdb/databasemsg.h"
#include "face/facedialog.h"
#include "screenshotoverlay.h"
#include "chathistorydialog.h"
#include "audioutil.h"
#include "audiorecorddialog.h"
#include <QToolButton>
#include <QTimer>
#include <QRandomGenerator>
#include <QImage>
#include <QPainter>
#include <QDateTime>

/**
 * @brief 内嵌图片落盘，实现放在 myHelper 里，主窗口收消息时也复用同一套逻辑
 */
static QString storeInlineImage(const QString &msg)
{
    return myHelper::SaveInlineImage(msg);
}

/**
 * @brief 语音消息落盘（b64 -> wav 文件）
 */
static QString storeInlineAudio(const QString &msg)
{
    return myHelper::SaveInlineAudio(msg);
}

/**
 * @brief 对端没带 dur 字段时，从 wav 数据里解析时长兜底
 */
static int getDurationOf(const QString &msg)
{
    if (!msg.startsWith("b64:")) return 0;
    const QByteArray wav = QByteArray::fromBase64(msg.mid(4).toLatin1());
    return AudioRecorder::wavDurationMs(wav);
}

ChatWindow::ChatWindow(CustomMoveWidget *parent) : CustomMoveWidget(parent),
    ui(new Ui::ChatWindow)
{
    ui->setupUi(this);
    setQss(":/qss/resource/qss/default.css");
    setWindowFlags(Qt::FramelessWindowHint);
    ui->widgetFileBoard->hide();

    QMenu *menu = new QMenu(this);
    QAction *actionEnter = menu->addAction(QIcon(""), tr("按Enter键发送"));
    QAction *actionCtrlEnter = menu->addAction(QIcon(""), tr("按Ctrl+Enter键发送"));

    QActionGroup *actionGroup = new QActionGroup(this);
    actionGroup->addAction(actionEnter);
    actionGroup->addAction(actionCtrlEnter);
    actionEnter->setCheckable(true);
    actionCtrlEnter->setCheckable(true);
    actionEnter->setChecked(true); // 默认
    m_isEnterSend = true;

    ui->btnAction->setMenu(menu);

    connect(actionEnter, &QAction::triggered, this, [=]()
    {
        m_isEnterSend = true;
        qDebug() << "切换为：按Enter键发送消息"; });
    connect(actionCtrlEnter, &QAction::triggered, this, [=]()
    {
        m_isEnterSend = false;
        qDebug() << "切换为：按Ctrl+Enter键发送消息"; });

    // 快捷键
    ui->btnSendMsg->setShortcut(QKeySequence("alt+s"));
    ui->btnClose->setShortcut(QKeySequence("alt+c"));

    ui->textEditMsg->setFocus();
    ui->textEditMsg->installEventFilter(this);

    m_nHistoryPage = 0;
    setAttribute(Qt::WA_DeleteOnClose); // 关闭窗口即释放内存
    connect(ui->widgetBubble, SIGNAL(signalWheelUp()), this, SLOT(sltWheelUp()));
    connect(ui->widgetBubble, &BubbleList::signalDownloadFile, this, &ChatWindow::sltDownloadFile);
    // 右键菜单：复制 / 撤回 / 删除 / 重新发送
    connect(ui->widgetBubble, &BubbleList::signalCopyText, this, &ChatWindow::sltCopyText);
    connect(ui->widgetBubble, &BubbleList::signalRecallMsg, this, &ChatWindow::sltRecallMsg);
    connect(ui->widgetBubble, &BubbleList::signalDeleteMsg, this, &ChatWindow::sltDeleteMsg);
    connect(ui->widgetBubble, &BubbleList::signalResendMsg, this, &ChatWindow::sltResendMsg);
    // 点语音气泡播放
    connect(ui->widgetBubble, &BubbleList::signalPlayAudio, this, &ChatWindow::sltPlayAudio);

    // 语音：工具栏按钮（.ui 里没有，代码插在截屏按钮后面）
    m_btnAudio = new QToolButton(this);
    m_btnAudio->setToolTip(tr("发送语音（点击开始，再点一次发送）"));
    m_btnAudio->setText(QString());
    m_btnAudio->setIcon(QIcon(":/resource/common/ic_record.png"));
    if (ui->toolButton_6)
    {
        const int idx = ui->horizontalLayout_3->indexOf(ui->toolButton_6);
        ui->horizontalLayout_3->insertWidget(idx + 1, m_btnAudio);
    }
    else
    {
        ui->horizontalLayout_3->insertWidget(0, m_btnAudio);
    }
    connect(m_btnAudio, &QToolButton::clicked, this, &ChatWindow::sltAudioBtnClicked);

    m_recorder = new AudioRecorder(this);
    connect(m_recorder, &AudioRecorder::signalError, this, [this](const QString &msg)
    {
        QMessageBox::warning(this, tr("语音"), msg);
    });

    m_audioTimer = new QTimer(this);
    m_audioTimer->setInterval(100);
    connect(m_audioTimer, &QTimer::timeout, this, &ChatWindow::sltAudioTick);

    // 等服务端回执的看门狗
    m_ackTimer = new QTimer(this);
    m_ackTimer->setInterval(1000);
    connect(m_ackTimer, &QTimer::timeout, this, &ChatWindow::sltAckCheckTimeout);

    // 播放结束：清掉气泡上的"播放中"高亮
    connect(AudioPlayer::instance(), &AudioPlayer::signalFinished,
            this, &ChatWindow::sltAudioFinished);
    connect(AudioPlayer::instance(), &AudioPlayer::signalError, this,
            [this](const QString &msg)
    {
        m_playingKey.clear();
        ui->widgetBubble->setPlayingKey(QString());
        QMessageBox::warning(this, tr("语音"), tr("语音播放失败：%1").arg(msg));
    });

    m_isGroup = false;

    // 聊天外观（字体 / 颜色）与输入长度统计
    loadChatAppearance();
    connect(ui->textEditMsg, &QTextEdit::textChanged, this, &ChatWindow::sltMsgTextChanged);
    sltMsgTextChanged();
}

ChatWindow::~ChatWindow()
{
    // 关窗口就停止播放，否则会留下"幽灵语音"
    if (!m_playingKey.isEmpty())
    {
        AudioPlayer::instance()->stop();
    }
    if (m_audioTimer) m_audioTimer->stop();
    if (m_ackTimer)   m_ackTimer->stop();
    if (m_recorder)   m_recorder->cancel();
    if (m_audioDialog) { delete m_audioDialog; m_audioDialog = nullptr; }

    // 会话列表里打开聊天时传入的是本窗口自己 new 出来的 QQCell，这里负责释放
    if (m_ownCell && m_cell)
    {
        delete m_cell;
        m_cell = nullptr;
    }

    delete ui;
}

void ChatWindow::setCellOwned(QQCell *cell)
{
    m_ownCell = true;
    setCell(cell);
}

void ChatWindow::setCell(QQCell *cell, const quint8 &type)
{
    if (!m_cell)
    {
        qDebug() << "setCell为空";
    }
    m_cell = cell;
    ui->labelWinTitle->setText(QString("与 %1 聊天中").arg(cell->name));
    this->setWindowTitle(QString("与 %1 聊天中").arg(cell->name));
    ui->widgetBubble->addItems(getHistoryMsg());

    m_nChatType = type;

    // 打开聊天窗口即视为已读，清掉该会话的未读标记
    DatabaseMsg::getInstance()->markSessionRead(cell->id, getIsGroup());
    emit signalSessionChanged();

    //        if (0 == type) {
    //            // 加载历史
    //            ui->widgetBubble->addItems(DataBaseMagr::Instance()->QueryHistory(m_cell->id, 10));
    //            ui->tableViewGroups->setVisible(false);
    //            ui->widgetFileInfo->setVisible(true);
    //            ui->widgetFileBoard->setVisible(false);
    //            // 链接文件服务器,方便下载文件
    //            m_tcpFileSocket->ConnectToServer(MyApp::m_strHostAddr, MyApp::m_nFilePort, m_cell->id);
    //        }
    //        else {
    //            ui->tableViewGroups->setVisible(true);
    //            ui->widgetFileInfo->setVisible(false);
    //            ui->widgetFileBoard->setVisible(true);
    //            // 添加群组人员
    //            m_model->setColumnCount(2);
    //            m_model->setRowCount(3);
    //            m_model->setHorizontalHeaderLabels(QStringList() << "好友" << "状态");
    //        }
}

int ChatWindow::getUserId()
{
    if (!m_cell)
    {
        qCritical() << "[ChatWindow] getUserId: m_cell是空指针！";
        return -1; // 返回无效ID，避免崩溃
    }
    return m_cell->id;
}

QString ChatWindow::getUserName()
{
    if (!m_cell)
    {
        qCritical() << "[ChatWindow] getUserName: m_cell是空指针！";
        return "-1"; // 返回无效ID，避免崩溃
    }
    return m_cell->name;
}

QString ChatWindow::getUserHead()
{
    if (!m_cell)
    {
        qCritical() << "[ChatWindow] getUserId: m_cell是空指针！";
        return "-1"; // 返回无效ID，避免崩溃
    }
    return m_cell->iconPath;
}

void ChatWindow::AddMessage(const QJsonValue &jsonVal) // 接收消息
{
    QJsonObject jsonObj = jsonVal.toObject();
    QString msg = jsonObj.value("msg").toString();
    int type = jsonObj.value("type").toInt();
    // 兼容旧服务端：type 丢失时图片会被当作文字，按内容前缀纠正
    if (static_cast<quint8>(type) == Text && msg.startsWith("b64:") && msg.size() > 256) {
        type = Picture;
    }
    if(type == Face && getIsGroup()) {
        // msg 是解密出来的下标字符串（"74"）。这里必须走 QString::toInt()：
        // QJsonValue::toInt() 只认 JSON 数字，遇到字符串一律返回 0，
        // 用它取值会让群表情在接收方永远显示第 1 张（下标 0）。
        const QJsonValue faceVal = jsonObj.value("msg");
        msg = QString::number(faceVal.isDouble() ? faceVal.toInt() : msg.toInt());
    }
    QString userName = jsonObj.value("name").toString();
    QString head = jsonObj.value("head").toString();
    ItemInfo *itemInfo = new ItemInfo(this);
    if(getIsGroup()){
        itemInfo->SetName(userName);
    } else {
        itemInfo->SetName(m_cell->name);
    }

    itemInfo->SetDatetime(QDateTime::currentDateTime().toString("MM-dd HH:mm"));
    itemInfo->SetHeadPixmap(MyApp::HeadFullPath(head));
    // 发送方自定义过颜色的话，用同样的颜色显示
    itemInfo->SetTextColor(jsonObj.value("color").toString());
    if (Text == static_cast<quint8>(type)) {
        itemInfo->SetText(msg);
    } else if (Face == static_cast<quint8>(type)) {
        itemInfo->SetFace(msg.toInt());
    } else if (Picture == static_cast<quint8>(type)) {
        // 对端发来的是内嵌 base64，落盘后气泡才能复用"路径型图片"的打开/另存逻辑
        itemInfo->SetText(storeInlineImage(msg));
    } else if (Audio == static_cast<quint8>(type)) {
        // 语音：同样先落盘成 wav，气泡才能直接播放
        itemInfo->SetText(storeInlineAudio(msg));
        itemInfo->SetDuration(jsonObj.value("dur").toInt(getDurationOf(msg)));
    } else if (Files == static_cast<quint8>(type))
    {
        itemInfo->SetText(msg);
        itemInfo->SetFileSizeString(jsonObj.value("size").toString());
    }
    qDebug() << "AddMessage【chatwindow144】" << "type" << type << "内容长度" << msg.size();
    itemInfo->SetMsgType(static_cast<quint8>(type));
    itemInfo->SetOrientation(Left);
    // 对端发来时就带着 key，撤回时能对上；老消息没有 key 就现场补一个
    const QString inKey = jsonObj.value("key").toString();
    itemInfo->SetMsgKey(inKey.isEmpty() ? newMsgKey() : inKey);
    itemInfo->SetTimeStamp(QDateTime::currentMSecsSinceEpoch());

    ui->widgetBubble->addItem(itemInfo);
    if(getIsGroup()) {
        int userId = jsonObj.value("userId").toInt();
        int groupId = jsonObj.value("id").toInt();
        DatabaseMsg::getInstance()->AddGroupHistoryMsg(userId, groupId, itemInfo);
    }else {
        DatabaseMsg::getInstance()->AddHistoryMsg(m_cell->id, itemInfo);
    }

    // 只有窗口真的在前台（用户正在看）才算已读；
    // 窗口在后台/最小化时保留未读，主窗口才会显示角标
    if (isActiveWindow() && isVisible()) {
        DatabaseMsg::getInstance()->markSessionRead(getUserId(), getIsGroup());
    }
    emit signalSessionChanged();
}

QVector<ItemInfo *> ChatWindow::getHistoryMsg()
{
    QVector<ItemInfo *> itemArr;
    QVector<QJsonObject> vJsonObj;
    //    name, head, datetime, fizesize, content, type, direction
    if(getIsGroup()){
        qDebug() << "获取群组聊天记录";
        vJsonObj = DatabaseMsg::getInstance()->getGroupHistoryMsg(getUserId(), m_nHistoryPage++);
    } else{
        vJsonObj = DatabaseMsg::getInstance()->getHistoryMsg(m_cell->id, m_nHistoryPage++);
    }
    foreach (QJsonObject obj, vJsonObj)
    {
        ItemInfo *item = new ItemInfo(this);
        item->SetName(obj.value("name").toString());
        item->SetHeadPixmap(MyApp::HeadFullPath(obj.value("head").toString()));
        item->SetDatetime(obj.value("datetime").toString());
        item->SetFileSizeString(obj.value("filesize").toString());
        item->SetMsgType(static_cast<quint8>(obj.value("type").toInt()));
        item->SetTextColor(obj.value("color").toString());
        item->SetMsgKey(obj.value("msg_key").toString());
        item->SetTimeStamp(obj.value("ts").toVariant().toLongLong());
        if (item->GetMsgType() == Text || item->GetMsgType() == Files
                || item->GetMsgType() == Picture || item->GetMsgType() == Audio)
        {
            item->SetText(obj.value("content").toString());
        }
        if (item->GetMsgType() == Audio)
        {
            item->SetDuration(obj.value("duration").toInt());
        }
        else if (item->GetMsgType() == Face)
        {
            item->SetFace(obj.value("content").toString().toInt());
        }
        if(getIsGroup()){
            int userId = obj.value("user_id").toInt();
            item->SetOrientation(userId != MyApp::m_nId ? Left : Right);
        } else {
            item->SetOrientation(m_cell->name == obj.value("name").toString() ? Left : Right);
        }

        itemArr.append(item);
    }
    return itemArr;
}

void ChatWindow::SetIsGroup()
{
    m_isGroup = true;
}

bool ChatWindow::getIsGroup()
{
    return m_isGroup;
}

void ChatWindow::sltWheelUp()
{
    // 加载更早的历史，追加到列表末尾，不要重建列表
    ui->widgetBubble->appendItems(getHistoryMsg());
}

void ChatWindow::sendFaceMsg(int index)
{
    const QString msgKey = newMsgKey();
    QJsonObject jsonObj;
    jsonObj.insert("id", getUserId());
    // 表情下标按字符串存放：正文统一加密后，密文解密回来是文本，接收端 toInt() 读取
    jsonObj.insert("msg", QString::number(index));
    jsonObj.insert("type", Face);
    jsonObj.insert("key", msgKey);
    qDebug() << "发送信号";
    if(getIsGroup()){
        emit signalSendMessage(SendGroupMsg, QJsonValue(jsonObj));
    } else {
        emit signalSendMessage(SendFace, QJsonValue(jsonObj));
    }
    beginSendTracking(msgKey, jsonObj);

    ItemInfo *itemInfo = new ItemInfo(this);
    itemInfo->SetName(MyApp::m_strUserName);
    itemInfo->SetDatetime(QDateTime::currentDateTime().toString("MM-dd HH:mm"));
    itemInfo->SetHeadPixmap(MyApp::HeadFullPath(MyApp::m_strHeadFile));
    itemInfo->SetSendState(SendSending);
    itemInfo->SetFace(index);
    itemInfo->SetOrientation(Right);
    // 表情只认 SetFace 的下标（0 基，对应资源 1.gif~132.gif），不再往 Text 里塞数字：
    // 以前发送方写 index+1、接收方写 index，两边口径不一致会画出不同的图。
    itemInfo->SetMsgType(Face);
    itemInfo->SetMsgKey(msgKey);
    itemInfo->SetTimeStamp(QDateTime::currentMSecsSinceEpoch());

    ui->widgetBubble->addItem(itemInfo);
    ui->textEditMsg->clear();
    if(getIsGroup()){
        DatabaseMsg::getInstance()->AddGroupHistoryMsg(MyApp::m_nId, getUserId(), itemInfo);
    } else {
        DatabaseMsg::getInstance()->AddHistoryMsg(m_cell->id, itemInfo);
    }
    emit signalSessionChanged();
}

void ChatWindow::sltDownloadFile(const QString &fileName)
{
    qDebug() << "触发文件下载，文件名：" << fileName;

    // 创建子线程和FileSocket对象
    QThread *fileRecvThread = new QThread();
    FileSocket *work = new FileSocket();
    work->moveToThread(fileRecvThread); // 移到子线程
    work->setUserId(QString::number(MyApp::m_nId)); // 设置当前用户ID
    work->setSavePath(MyApp::m_strRecvPath); // 设置接收保存路径
    // 下载回来的可能是密文：把本机私钥交给工作线程，用于从文件头解出会话密钥
    work->setDecryptIdentity(MyApp::m_nId, KeyStore::instance()->localPrivKey());

    // 显示文件进度面板
    ui->widgetFileInfo->show();
    ui->widgetFileBoard->show();
    ui->progressBar->setValue(0); // 重置进度条

    // 连接信号槽
    // 主线程 → 子线程：触发连接服务器
    connect(this, &ChatWindow::signalConnectFileServer,
            work, &FileSocket::connectToFileServer, Qt::QueuedConnection);

    // 子线程 → 主线程：进度更新
    connect(work, &FileSocket::progressUpdated, this, [=](quint64 processed, quint64 total) {
        if (total == 0) return;

        // 计算百分比
        int progress = static_cast<int>((processed * 100) / total);

        //仅数值变化时更新UI，避免高频刷新卡顿
        static quint64 lastProcessed = 0;
        static int lastProgress = 0;
        if (processed == lastProcessed && progress == lastProgress) return;

        // 批量更新UI
        ui->widgetFileBoard->setUpdatesEnabled(false);
        // 调用myHelper::CalcSize格式化大小（GB/MB/KB）
        ui->lineEditTotalSize->setText(myHelper::CalcSize(static_cast<qint64>(total)));
        ui->lineEditCurrSize->setText(myHelper::CalcSize(static_cast<qint64>(processed)));
        ui->progressBar->setValue(progress);
        ui->widgetFileBoard->setUpdatesEnabled(true);

        // 更新最后一次值
        lastProcessed = processed;
        lastProgress = progress;
    }, Qt::QueuedConnection);

    // 子线程 → 主线程：下载完成
    connect(work, &FileSocket::recvFinished, this, [=](const QString &savePath) {
        QMessageBox::information(this, tr("文件下载"),
                                 tr("文件下载成功！保存路径：\n%1").arg(savePath));
        ui->widgetFileBoard->hide();
        // 气泡是按"接收目录里的真实文件"判断状态的，重绘一次它才会从
        // "文件未下载"变成文件大小 + 系统图标
        ui->widgetBubble->update();
        // 停止线程并清理
        fileRecvThread->quit();
    }, Qt::QueuedConnection);

    // 子线程 → 主线程：下载失败
    connect(work, &FileSocket::sendFailed, this, [=](const QString &errMsg) {
        QMessageBox::critical(this, tr("文件下载失败"), errMsg);
        ui->widgetFileBoard->hide();
        // 停止线程并清理
        fileRecvThread->quit();
    }, Qt::QueuedConnection);

    // 子线程 → 主线程：连接成功后触发下载请求
    connect(work, &FileSocket::fileServerConnected, this, [=]() {
        work->requestFile(fileName); // 发送下载请求
    }, Qt::QueuedConnection);

    connect(fileRecvThread, &QThread::finished, work, &FileSocket::deleteLater, Qt::QueuedConnection);
    connect(fileRecvThread, &QThread::finished, fileRecvThread, &QThread::deleteLater, Qt::QueuedConnection);

    // 启动线程 → 触发连接服务器
    fileRecvThread->start();
    emit signalConnectFileServer(MyApp::m_strHostAddr, MyApp::m_nFilePort);
}
void ChatWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    switch (event->type())
    {
    case QEvent::LanguageChange:
        ui->retranslateUi(this);
        break;
    default:
        break;
    }
}

/**
 * @brief ChatWindow::event
 * 用户切到本窗口（激活）时，把该会话的未读清掉
 */
bool ChatWindow::event(QEvent *e)
{
    if (QEvent::WindowActivate == e->type() && nullptr != m_cell)
    {
        DatabaseMsg *db = DatabaseMsg::getInstance();
        const int unread = getIsGroup() ? db->getUnreadCountByGroup(m_cell->id)
                                        : db->getUnreadCountByFriend(m_cell->id);
        if (unread > 0)
        {
            db->markSessionRead(m_cell->id, getIsGroup());
            emit signalSessionChanged();
        }
    }
    return CustomMoveWidget::event(e);
}

void ChatWindow::keyPressEvent(QKeyEvent *event)
{
}

bool ChatWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->textEditMsg && event->type() == QEvent::KeyPress)
    {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        bool isEnterKey = (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter);

        if (isEnterKey)
        {
            if (m_isEnterSend)
            {
                if (keyEvent->modifiers() == Qt::NoModifier || keyEvent->modifiers() == Qt::KeypadModifier)
                {
                    on_btnSendMsg_clicked();
                    return true;
                }

                else if (keyEvent->modifiers() == Qt::ShiftModifier)
                {
                    return false;
                }
            }
            else
            {

                if (keyEvent->modifiers() == Qt::ControlModifier)
                {
                    on_btnSendMsg_clicked();
                    return true;
                }
            }
        }
    }
    return CustomMoveWidget::eventFilter(watched, event);
}

void ChatWindow::on_btnSendMsg_clicked()
{
    QString msg = ui->textEditMsg->toPlainText().trimmed();

    if (msg.isEmpty())
    {
        QPoint point = ui->btnSendMsg->mapToGlobal(QPoint(0, -20));
        QToolTip::showText(point, tr("发送消息不能为空"));
        return;
    }
    if (msg.length() > m_maxMsgLength)
    {
        QPoint point = ui->btnSendMsg->mapToGlobal(QPoint(0, -20));
        QToolTip::showText(point, tr("消息过长：%1 / %2 字，请精简后再发")
                           .arg(msg.length()).arg(m_maxMsgLength));
        return;
    }
    const QString msgKey = newMsgKey();
    QJsonObject jsonObj;
    jsonObj.insert("id", getUserId());
    jsonObj.insert("msg", msg);
    jsonObj.insert("type", Text);
    jsonObj.insert("key", msgKey);          // 撤回时双方靠它定位同一条消息
    // 只有用户真的挑过颜色才带这个字段，对方按同样颜色显示
    if (m_textColor.isValid())
    {
        jsonObj.insert("color", m_textColor.name());
    }
    qDebug() << "发送信号";
    if(getIsGroup()){
        emit signalSendMessage(SendGroupMsg, QJsonValue(jsonObj));
    } else {
        emit signalSendMessage(SendMsg, QJsonValue(jsonObj));
    }
    beginSendTracking(msgKey, jsonObj);

    ItemInfo *itemInfo = new ItemInfo(this);
    itemInfo->SetName(MyApp::m_strUserName);
    itemInfo->SetDatetime(DATE_TIME);
    itemInfo->SetHeadPixmap(MyApp::HeadFullPath(MyApp::m_strHeadFile));
    itemInfo->SetText(msg);
    itemInfo->SetOrientation(Right);
    itemInfo->SetMsgType(Text);
    itemInfo->SetMsgKey(msgKey);
    itemInfo->SetTimeStamp(QDateTime::currentMSecsSinceEpoch());
    itemInfo->SetSendState(SendSending);
    if (m_textColor.isValid())
    {
        itemInfo->SetTextColor(m_textColor.name());
    }

    ui->widgetBubble->addItem(itemInfo);
    ui->textEditMsg->clear();
    if(getIsGroup()){
        DatabaseMsg::getInstance()->AddGroupHistoryMsg(MyApp::m_nId, getUserId(), itemInfo);
    } else {
        DatabaseMsg::getInstance()->AddHistoryMsg(m_cell->id, itemInfo);
    }
    emit signalSessionChanged();
}

void ChatWindow::on_btnSendFile_clicked()
{
    // 选择文件
    QFileDialog fileDialog(this);
    fileDialog.setWindowTitle(tr("选择要发送的文件"));
    fileDialog.setDirectory(QDir::homePath());
    fileDialog.setNameFilter(tr("所有文件 (*.*)"));
    fileDialog.setFileMode(QFileDialog::ExistingFile);
    fileDialog.setViewMode(QFileDialog::Detail);

    QString filePath = fileDialog.getOpenFileName();
    if (filePath.isEmpty())
    {
        qDebug() << "未选择任何文件，取消发送";
        return;
    }

    // 获取文件信息
    QFileInfo fileInfo(filePath);
    QString fileName = fileInfo.fileName();
    qint64 fileSize = fileInfo.size();
    QString fileSizeStr = myHelper::CalcSize(fileSize); // 格式化文件大小

    // ===== 端到端加密：为这次文件传输生成一次性会话密钥 =====
    // 密钥用接收者的 RSA 公钥包裹后写进文件头，服务端只是把它当字节搬运，
    // 落到服务器磁盘上的永远是密文，只有接收者能解开。
    QByteArray fileKey, fileIv, fileMeta;
    if (!getIsGroup())
    {
        KeyStore *ks = KeyStore::instance();
        const int peerId = getUserId();
        if (!ks->isReady() || !ks->hasPubKey(peerId))
        {
            QMessageBox::warning(this, tr("端到端加密"),
                                 tr("对方公钥尚未就绪，暂时无法发送加密文件。\n"
                                    "请确认对方已登录过本客户端后再试。"));
            return;
        }

        fileKey = KeyStore::randomSessionKey(32);
        fileIv  = KeyStore::randomSessionKey(16);
        QByteArray wrapped;
        if (!CryptoUtil::wrap(ks->pubKey(peerId), fileKey + fileIv, &wrapped))
        {
            QMessageBox::warning(this, tr("端到端加密"),
                                 tr("文件会话密钥生成失败：%1").arg(CryptoUtil::lastError()));
            return;
        }

        QJsonObject eks;
        eks.insert(QString::number(peerId), QString::fromLatin1(wrapped.toBase64()));
        QJsonObject meta;
        meta.insert("v", 1);
        meta.insert("alg", "AES-256-CTR");
        meta.insert("eks", eks);
        fileMeta = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    }

    // 构造文件消息JSON（通知接收方）
    QJsonObject jsonObj;
    jsonObj.insert("id", MyApp::m_nId); // 发送方ID
    jsonObj.insert("to", getUserId());   // 接收方ID
    jsonObj.insert("msg", fileName);    // 文件名
    jsonObj.insert("size", fileSizeStr); // 格式化后的文件大小
//    jsonObj.insert("head", MyApp::m_strHeadFile);
    jsonObj.insert("type", Files); // 消息类型：文件
    const QString msgKey = newMsgKey();
    jsonObj.insert("key", msgKey);
    if(getIsGroup()){
        emit signalSendMessage(SendGroupMsg, jsonObj);
    } else {
        emit signalSendMessage(SendFile, jsonObj);
    }
    beginSendTracking(msgKey, jsonObj);

    // 本地UI显示文件消息
    ItemInfo *itemInfo = new ItemInfo(this);
    itemInfo->SetName(MyApp::m_strUserName);
    itemInfo->SetDatetime(QDateTime::currentDateTime().toString("MM-dd HH:mm"));
    itemInfo->SetHeadPixmap(MyApp::HeadFullPath(MyApp::m_strHeadFile));
    itemInfo->SetText(fileName); // 显示文件名
    itemInfo->SetFileSizeString(fileSizeStr); // 显示格式化后的文件大小
    itemInfo->SetOrientation(Right);
    itemInfo->SetMsgType(Files); // 消息类型：文件
    itemInfo->SetMsgKey(msgKey);
    itemInfo->SetSendState(SendSending);
    itemInfo->SetTimeStamp(QDateTime::currentMSecsSinceEpoch());

    ui->widgetBubble->addItem(itemInfo);
    itemInfo->SetText(filePath);
    if(getIsGroup()){
        DatabaseMsg::getInstance()->AddGroupHistoryMsg(MyApp::m_nId, getUserId(), itemInfo);
    } else {
        DatabaseMsg::getInstance()->AddHistoryMsg(m_cell->id, itemInfo);
    }
    emit signalSessionChanged();

    // 显示文件发送进度面板
    ui->widgetFileBoard->show();
    ui->progressBar->setValue(0);

    // 创建子线程和FileSocket
    QThread *fileSendThread = new QThread();
    FileSocket *work = new FileSocket();
    work->moveToThread(fileSendThread);
    work->setFilePath(filePath);       // 设置发送文件路径
    work->setLoadSize(8 * 1024);       // 8KB分片
    work->setUserId(QString::number(MyApp::m_nId)); // 设置用户ID
    // 私聊文件走端到端加密；群文件当前只发元信息，不经过文件通道
    if (!fileMeta.isEmpty())
        work->setEncryptSession(fileKey, fileIv, fileMeta);

    // 信号连接
    // 主线程 → 子线程：连接服务器、启动发送
    connect(this, &ChatWindow::signalConnectFileServer, work, &FileSocket::connectToFileServer);
    connect(this, &ChatWindow::signalStartSend, work, &FileSocket::startSendFile);

    // 子线程 → 主线程：进度更新（UI显示）
    connect(work, &FileSocket::progressUpdated, this, [=](quint64 sent, quint64 total) {
        if (total == 0) return;
        int progress = static_cast<int>((sent * 100) / total);

        // 节流：仅变化时更新
        static quint64 lastSent = 0;
        static int lastProgress = 0;
        if (sent == lastSent && progress == lastProgress) return;

        // 批量更新UI，大小显示调用CalcSize
        ui->widgetFileBoard->setUpdatesEnabled(false);
        ui->lineEditTotalSize->setText(myHelper::CalcSize(static_cast<qint64>(total)));
        ui->lineEditCurrSize->setText(myHelper::CalcSize(static_cast<qint64>(sent)));
        ui->progressBar->setValue(progress);
        ui->widgetFileBoard->setUpdatesEnabled(true);

        lastSent = sent;
        lastProgress = progress;
    }, Qt::QueuedConnection);

    // 子线程 → 主线程：发送完成
    connect(work, &FileSocket::sendFinished, this, [=]() {
        QMessageBox::information(this, tr("文件发送"), tr("文件「%1」发送成功！").arg(fileName));
        ui->widgetFileBoard->hide();
        fileSendThread->quit();
    }, Qt::QueuedConnection);

    // 子线程 → 主线程：发送失败
    connect(work, &FileSocket::sendFailed, this, [=](const QString &errMsg) {
        QMessageBox::critical(this, tr("文件发送失败"), errMsg);
        ui->widgetFileBoard->hide();
        fileSendThread->quit();
    }, Qt::QueuedConnection);

    // 连接成功后自动启动发送
    connect(work, &FileSocket::fileServerConnected, this, &ChatWindow::signalStartSend);

    // 线程结束清理
    connect(fileSendThread, &QThread::finished, work, &FileSocket::deleteLater);
    connect(fileSendThread, &QThread::finished, fileSendThread, &QThread::deleteLater);

    // 启动线程并连接服务器
    fileSendThread->start();
    emit signalConnectFileServer(MyApp::m_strHostAddr, MyApp::m_nFilePort);

    qDebug() << "开始发送文件：" << fileName << "大小：" << fileSizeStr;
}
// fileName
void ChatWindow::on_btnClose_clicked()
{
    emit signalClose();
    close();
}

void ChatWindow::on_btnWinClose_clicked()
{
    emit signalClose();
    close();
}

void ChatWindow::on_toolButton_3_clicked()
{
    faceDialog *face = new faceDialog;
    face->show();
    connect(this, &ChatWindow::signalClose, face, &faceDialog::sltClose);
    connect(face, &faceDialog::signalSelectFaceIndex, this, &ChatWindow::sendFaceMsg);
}

/**
 * @brief ChatWindow::loadChatAppearance
 * 读取上次使用的聊天字体与文字颜色（保存在 config.ini 的 [Chat] 段）
 */
void ChatWindow::loadChatAppearance()
{
    QSettings settings(MyApp::m_strIniFile, QSettings::IniFormat);

    settings.beginGroup("Chat");
    const QString family = settings.value("FontFamily").toString();
    const int     size   = settings.value("FontSize", 0).toInt();
    const bool    bold   = settings.value("FontBold", false).toBool();
    const bool    italic = settings.value("FontItalic", false).toBool();
    const QString color  = settings.value("TextColor").toString();
    settings.endGroup();

    if (!family.isEmpty() && size > 0)
    {
        m_chatFont = QFont(family, size);
    }
    else
    {
        // 与气泡控件默认值保持一致
        m_chatFont = QFont("楷体", 12);
    }
    m_chatFont.setBold(bold);
    m_chatFont.setItalic(italic);

    m_textColor = color.isEmpty() ? QColor() : QColor(color);

    applyChatFont();
}

/**
 * @brief ChatWindow::saveChatAppearance
 * 把当前字体 / 颜色写回配置，下次打开聊天窗口自动生效
 */
void ChatWindow::saveChatAppearance()
{
    QSettings settings(MyApp::m_strIniFile, QSettings::IniFormat);

    settings.beginGroup("Chat");
    settings.setValue("FontFamily", m_chatFont.family());
    settings.setValue("FontSize",   m_chatFont.pointSize());
    settings.setValue("FontBold",   m_chatFont.bold());
    settings.setValue("FontItalic", m_chatFont.italic());
    settings.setValue("TextColor",  m_textColor.isValid() ? m_textColor.name() : QString());
    settings.endGroup();
}

void ChatWindow::applyChatFont()
{
    ui->textEditMsg->setFont(m_chatFont);
    // 新输入的文字直接用用户选的颜色（选中已有文字时应只影响后续输入）
    ui->textEditMsg->setTextColor(m_textColor.isValid() ? m_textColor : QColor("#000000"));
    ui->widgetBubble->setChatFont(m_chatFont);
}

/**
 * @brief ChatWindow::on_toolButton_2_clicked
 * 修改聊天字体：同时作用于输入框和消息区，并记住这次选择
 */
void ChatWindow::on_toolButton_2_clicked()
{
    bool ok = false;
    const QFont font = QFontDialog::getFont(&ok, m_chatFont, this, tr("选择聊天字体"));
    if (!ok) return;

    m_chatFont = font;
    applyChatFont();
    saveChatAppearance();

    // 字体变了输入框高度也会变，滚动到底部避免内容被挡
    ui->textEditMsg->ensureCursorVisible();
}

/**
 * @brief ChatWindow::on_toolButton_4_clicked
 * 设置字体颜色：影响自己后续发送的文字消息
 */
void ChatWindow::on_toolButton_4_clicked()
{
    const QColor color = QColorDialog::getColor(
        m_textColor.isValid() ? m_textColor : QColor("#000000"),
        this, tr("选择字体颜色"));
    if (!color.isValid()) return;

    m_textColor = color;
    applyChatFont();
    saveChatAppearance();
}

/**
 * @brief ChatWindow::sltMsgTextChanged
 * 实时统计已输入字符数，超过上限时把提示变红
 */
void ChatWindow::sltMsgTextChanged()
{
    const int len = ui->textEditMsg->toPlainText().length();

    ui->label->setText(tr("字符限制: %1 / %2").arg(len).arg(m_maxMsgLength));

    const bool over = (len > m_maxMsgLength);
    if (over)
    {
        ui->label->setStyleSheet("color:#E64340;");
    }
    else
    {
        ui->label->setStyleSheet(QString());
    }
}

/**
 * @brief ChatWindow::on_toolButton_7_clicked
 * 发送图片：读图 -> 等比缩小 -> 压缩 -> base64 内嵌进消息
 * 走消息通道而不是文件通道，图片内容同样被端到端加密保护
 */
void ChatWindow::on_toolButton_7_clicked()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString filePath = QFileDialog::getOpenFileName(
        this, tr("选择要发送的图片"), dir,
        tr("图片文件 (*.png *.jpg *.jpeg *.bmp *.gif *.webp)"));
    if (filePath.isEmpty()) return;

    sendImageFile(filePath);
}

void ChatWindow::sendImageFile(const QString &filePath)
{
    QImageReader reader(filePath);
    reader.setAutoTransform(true);   // 手机照片按 EXIF 方向摆正
    const QImage img = reader.read();
    if (img.isNull())
    {
        QMessageBox::warning(this, tr("发送图片"),
                             tr("无法读取该图片：%1").arg(reader.errorString()));
        return;
    }
    sendImagePixmap(QPixmap::fromImage(img));
}

void ChatWindow::sendImagePixmap(const QPixmap &src)
{
    if (src.isNull()) return;

    QImage img = src.toImage();
    if (img.width() > kMaxImageSide || img.height() > kMaxImageSide)
    {
        img = img.scaled(kMaxImageSide, kMaxImageSide,
                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    // JPEG 不支持透明，先铺白底，否则截图里的透明区域会变黑
    if (img.hasAlphaChannel())
    {
        QImage flat(img.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter p(&flat);
        p.drawImage(0, 0, img);
        p.end();
        img = flat;
    }

    QByteArray raw;
    QBuffer buffer(&raw);
    buffer.open(QIODevice::WriteOnly);
    bool ok = img.save(&buffer, "JPG", 85);
    buffer.close();

    // 还是太大就降一档质量再压一次
    if (ok && raw.size() > kMaxImageBytes)
    {
        raw.clear();
        QBuffer buffer2(&raw);
        buffer2.open(QIODevice::WriteOnly);
        ok = img.save(&buffer2, "JPG", 68);
        buffer2.close();
    }

    if (!ok || raw.isEmpty())
    {
        QMessageBox::warning(this, tr("发送图片"), tr("图片压缩失败，请换一张试试"));
        return;
    }

    // 兜底上限：再大就不适合塞进聊天消息了
    if (raw.size() > 2 * 1024 * 1024)
    {
        QMessageBox::warning(this, tr("发送图片"),
                             tr("图片过大（压缩后 %1），请用「发送文件」发送")
                             .arg(myHelper::CalcSize(raw.size())));
        return;
    }

    const QString msgForSend = "b64:" + QString::fromLatin1(raw.toBase64());
    // 本地留一份，历史记录重开时直接读文件，不用反复解码
    const QString localPath = storeInlineImage(msgForSend);

    const QString msgKey = newMsgKey();
    QJsonObject jsonObj;
    jsonObj.insert("id", getUserId());
    jsonObj.insert("msg", msgForSend);
    jsonObj.insert("type", Picture);
    jsonObj.insert("key", msgKey);

    if (getIsGroup())
    {
        emit signalSendMessage(SendGroupMsg, QJsonValue(jsonObj));
    }
    else
    {
        emit signalSendMessage(SendMsg, QJsonValue(jsonObj));
    }
    beginSendTracking(msgKey, jsonObj);

    appendImageBubble(localPath.isEmpty() ? msgForSend : localPath, msgKey);
    qDebug() << "[图片] 已发送，压缩后" << myHelper::CalcSize(raw.size())
             << "原图" << src.width() << "x" << src.height()
             << "发送" << img.width() << "x" << img.height();
}

/**
 * @brief ChatWindow::on_toolButton_6_clicked
 * 截屏：先把自己藏起来，抓全屏后交给覆盖层框选，确认后按图片消息发出
 */
void ChatWindow::on_toolButton_6_clicked()
{
    const bool wasVisible = isVisible();
    hide();

    // 等一下让窗口真正消失，否则截图里会留着自己的残影
    QTimer::singleShot(250, this, [this, wasVisible]()
    {
        QRect desktopRect;
        const QPixmap desktop = ScreenshotOverlay::grabDesktop(&desktopRect);
        if (desktop.isNull())
        {
            if (wasVisible) { show(); raise(); activateWindow(); }
            QMessageBox::warning(this, tr("截屏"), tr("未能抓取屏幕内容"));
            return;
        }

        ScreenshotOverlay *overlay = new ScreenshotOverlay(desktop, desktopRect);
        connect(overlay, &ScreenshotOverlay::captured, this,
                [this, wasVisible](const QPixmap &pixmap)
        {
            if (wasVisible) { show(); raise(); activateWindow(); }
            sendImagePixmap(pixmap);
        });
        connect(overlay, &ScreenshotOverlay::cancelled, this, [this, wasVisible]()
        {
            if (wasVisible) { show(); raise(); activateWindow(); }
        });

        overlay->show();
        overlay->raise();
        overlay->activateWindow();
        overlay->setFocus();
    });
}

/**
 * @brief ChatWindow::on_toolButton_clicked
 * 聊天记录：把本地库里的完整对话列出来，支持关键字过滤
 */
void ChatWindow::on_toolButton_clicked()
{
    DatabaseMsg *db = DatabaseMsg::getInstance();
    QVector<QJsonObject> messages;
    QString title;

    if (getIsGroup())
    {
        title = tr("群「%1」的聊天记录").arg(getUserName());
        messages = db->getAllGroupHistoryMsg(getUserId());
    }
    else
    {
        title = tr("与「%1」的聊天记录").arg(getUserName());
        messages = db->getAllHistoryMsg(getUserId());
    }

    ChatHistoryDialog *dialog = new ChatHistoryDialog(title, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMessages(messages);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();

    qDebug() << "[聊天记录]" << title << "共" << messages.size() << "条";
}

/**
 * @brief ChatWindow::appendImageBubble
 * 在自己的消息区追加一条图片消息并写入本地历史
 */
void ChatWindow::appendImageBubble(const QString &localPath, const QString &msgKey)
{
    ItemInfo *itemInfo = new ItemInfo(this);
    itemInfo->SetName(MyApp::m_strUserName);
    itemInfo->SetDatetime(QDateTime::currentDateTime().toString("MM-dd HH:mm"));
    itemInfo->SetHeadPixmap(MyApp::HeadFullPath(MyApp::m_strHeadFile));
    itemInfo->SetText(localPath);
    itemInfo->SetMsgType(Picture);
    itemInfo->SetOrientation(Right);
    itemInfo->SetMsgKey(msgKey);
    itemInfo->SetTimeStamp(QDateTime::currentMSecsSinceEpoch());
    itemInfo->SetSendState(SendSending);

    ui->widgetBubble->addItem(itemInfo);

    if (getIsGroup())
    {
        DatabaseMsg::getInstance()->AddGroupHistoryMsg(MyApp::m_nId, getUserId(), itemInfo);
    }
    else
    {
        DatabaseMsg::getInstance()->AddHistoryMsg(getUserId(), itemInfo);
    }
    emit signalSessionChanged();
}

/**
 * @brief ChatWindow::newMsgKey
 * 生成消息唯一标识：收发双方用同一个 key，撤回时才能对上同一条消息
 */
QString ChatWindow::newMsgKey()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

/**
 * @brief ChatWindow::sltCopyText
 * 右键"复制"：文字进剪贴板
 */
void ChatWindow::sltCopyText(const QString &text)
{
    if (text.isEmpty()) return;
    QApplication::clipboard()->setText(text);
    QToolTip::showText(QCursor::pos(), tr("已复制到剪贴板"), this, QRect(), 1200);
    qDebug() << "[右键] 已复制" << text.size() << "字";
}

/**
 * @brief ChatWindow::sltRecallMsg
 * 右键"撤回"：通知对端删掉同一条消息，然后本地移除
 * 2 分钟时限在气泡菜单里已经拦过一次，这里不再重复判断
 */
void ChatWindow::sltRecallMsg(const QString &msgKey)
{
    if (msgKey.isEmpty()) return;

    QJsonObject json;
    json.insert("id", getUserId());        // 私聊=对方 id；群聊=群 id
    json.insert("key", msgKey);
    if (getIsGroup()) json.insert("group", 1);

    emit signalSendMessage(RecallMsg, QJsonValue(json));

    // 本地立即消失（含数据库），不等对端回执
    removeMsgByKey(msgKey, false);
    QToolTip::showText(QCursor::pos(), tr("已撤回"), this, QRect(), 1200);
    qDebug() << "[撤回]" << (getIsGroup() ? "群" : "私聊") << msgKey;
}

/**
 * @brief ChatWindow::sltDeleteMsg
 * 右键"删除"：只删本地，不动对端
 */
void ChatWindow::sltDeleteMsg(const QString &msgKey)
{
    removeMsgByKey(msgKey, true);
    QToolTip::showText(QCursor::pos(), tr("已删除"), this, QRect(), 1200);
    qDebug() << "[删除]" << msgKey;
}

/**
 * @brief ChatWindow::removeMsgByKey
 * 界面气泡 + 数据库一起清掉
 * @param fromPeer true=对端撤回触发的，本地删掉即可
 */
void ChatWindow::removeMsgByKey(const QString &msgKey, bool fromPeer)
{
    Q_UNUSED(fromPeer)
    if (msgKey.isEmpty()) return;

    const bool uiRemoved = ui->widgetBubble->removeItemByKey(msgKey);
    const bool dbRemoved = DatabaseMsg::getInstance()->deleteMsgByKey(msgKey);
    if (!uiRemoved && !dbRemoved)
    {
        qDebug() << "[消息] 没找到要移除的消息" << msgKey;
    }
    emit signalSessionChanged();
}

/**
 * @brief ChatWindow::sendAudioMessage
 * 语音：WAV -> base64 -> 消息通道（和图片一样走端到端加密）
 */
void ChatWindow::sendAudioMessage(const QByteArray &wav, int durationMs)
{
    if (wav.isEmpty()) return;

    const QString msgForSend = "b64:" + QString::fromLatin1(wav.toBase64());
    // 本地留一份，历史记录重开时直接读文件，不用反复解码
    const QString localPath = myHelper::SaveInlineAudio(msgForSend);

    const QString msgKey = newMsgKey();
    QJsonObject jsonObj;
    jsonObj.insert("id", getUserId());
    jsonObj.insert("msg", msgForSend);
    jsonObj.insert("type", Audio);
    jsonObj.insert("key", msgKey);
    jsonObj.insert("dur", durationMs);      // 秒数在气泡里显示，不必每次解析 wav

    if (getIsGroup())
    {
        emit signalSendMessage(SendGroupMsg, QJsonValue(jsonObj));
    }
    else
    {
        emit signalSendMessage(SendMsg, QJsonValue(jsonObj));
    }
    beginSendTracking(msgKey, jsonObj);

    appendAudioBubble(localPath.isEmpty() ? msgForSend : localPath, msgKey, durationMs);

    qDebug() << "[语音] 已发送 时长" << durationMs << "ms 大小"
             << myHelper::CalcSize(wav.size());
}

/**
 * @brief ChatWindow::appendAudioBubble
 * 自己的语音消息追加到消息区并写入本地历史
 */
void ChatWindow::appendAudioBubble(const QString &localPath, const QString &msgKey, int durationMs)
{
    ItemInfo *itemInfo = new ItemInfo(this);
    itemInfo->SetName(MyApp::m_strUserName);
    itemInfo->SetDatetime(QDateTime::currentDateTime().toString("MM-dd HH:mm"));
    itemInfo->SetHeadPixmap(MyApp::HeadFullPath(MyApp::m_strHeadFile));
    itemInfo->SetText(localPath);
    itemInfo->SetMsgType(Audio);
    itemInfo->SetDuration(durationMs);
    itemInfo->SetOrientation(Right);
    itemInfo->SetMsgKey(msgKey);
    itemInfo->SetTimeStamp(QDateTime::currentMSecsSinceEpoch());
    itemInfo->SetSendState(SendSending);

    ui->widgetBubble->addItem(itemInfo);

    if (getIsGroup())
    {
        DatabaseMsg::getInstance()->AddGroupHistoryMsg(MyApp::m_nId, getUserId(), itemInfo);
    }
    else
    {
        DatabaseMsg::getInstance()->AddHistoryMsg(getUserId(), itemInfo);
    }
    emit signalSessionChanged();
}

/**
 * @brief ChatWindow::sltAudioBtnClicked
 * 语音按钮：第一次点开始录音，再点一次结束并发送
 */
void ChatWindow::sltAudioBtnClicked()
{
    if (!m_recorder) return;

    if (m_recorder->isRecording())
    {
        sltAudioSend();
        return;
    }

    if (m_audioDialog)
    {
        delete m_audioDialog;
        m_audioDialog = nullptr;
    }

    if (!m_recorder->start()) return;      // 失败原因已经由 signalError 弹窗提示

    m_audioDialog = new AudioRecordDialog(this);
    connect(m_audioDialog, &AudioRecordDialog::signalSend,   this, &ChatWindow::sltAudioSend);
    connect(m_audioDialog, &AudioRecordDialog::signalCancel, this, &ChatWindow::sltAudioCancel);
    m_audioDialog->setElapsed(0);

    // 浮在语音按钮正上方；顶到屏幕外就翻到按钮下面
    const QPoint top = m_btnAudio->mapToGlobal(QPoint(0, 0));
    QPoint pos(top.x() - 120, top.y() - m_audioDialog->height() - 10);
    if (pos.y() < 0) pos.setY(top.y() + m_btnAudio->height() + 10);
    m_audioDialog->move(pos);
    m_audioDialog->show();
    m_audioDialog->raise();

    m_btnAudio->setToolTip(tr("点击结束并发送"));
    m_audioTimer->start(100);
}

/**
 * @brief ChatWindow::sltAudioTick
 * 刷新录音计时；到 60 秒自动结束并发送
 */
void ChatWindow::sltAudioTick()
{
    if (!m_recorder || !m_recorder->isRecording())
    {
        m_audioTimer->stop();
        return;
    }

    const int ms = m_recorder->elapsedMs();
    if (m_audioDialog) m_audioDialog->setElapsed(ms);

    if (ms >= kMaxAudioSec * 1000)
    {
        sltAudioSend();
    }
}

void ChatWindow::sltAudioSend()
{
    m_audioTimer->stop();
    if (m_btnAudio) m_btnAudio->setToolTip(tr("发送语音（点击开始，再点一次发送）"));

    if (!m_recorder || !m_recorder->isRecording())
    {
        closeAudioDialog();
        return;
    }

    const QByteArray wav = m_recorder->stop();
    closeAudioDialog();

    if (wav.isEmpty())
    {
        QToolTip::showText(QCursor::pos(), tr("录音时间太短，已取消"), this, QRect(), 1500);
        return;
    }

    if (wav.size() > 2 * 1024 * 1024)
    {
        QMessageBox::warning(this, tr("语音"),
                             tr("语音过长（%1），请控制在 %2 秒以内")
                             .arg(myHelper::CalcSize(wav.size())).arg(kMaxAudioSec));
        return;
    }

    const int dur = AudioRecorder::wavDurationMs(wav);
    sendAudioMessage(wav, dur > 0 ? dur : kMaxAudioSec * 1000);
}

void ChatWindow::sltAudioCancel()
{
    m_audioTimer->stop();
    if (m_recorder) m_recorder->cancel();
    closeAudioDialog();
    if (m_btnAudio) m_btnAudio->setToolTip(tr("发送语音（点击开始，再点一次发送）"));

    QToolTip::showText(QCursor::pos(), tr("已取消录音"), this, QRect(), 1200);
}

void ChatWindow::closeAudioDialog()
{
    if (!m_audioDialog) return;
    m_audioDialog->hide();
    m_audioDialog->deleteLater();
    m_audioDialog = nullptr;
}

/**
 * @brief ChatWindow::sltPlayAudio
 * 点气泡播放语音；再点同一条就是停止
 */
void ChatWindow::sltPlayAudio(const QString &msgKey, const QString &path)
{
    QString local = path;
    if (local.startsWith("b64:")) local = myHelper::SaveInlineAudio(local);
    if (local.isEmpty()) return;

    if (m_playingKey == msgKey && AudioPlayer::instance()->isPlaying())
    {
        AudioPlayer::instance()->stop();
        m_playingKey.clear();
        ui->widgetBubble->setPlayingKey(QString());
        return;
    }

    m_playingKey = msgKey;
    ui->widgetBubble->setPlayingKey(msgKey);
    AudioPlayer::instance()->play(local);
}

void ChatWindow::sltAudioFinished(const QString &path)
{
    Q_UNUSED(path)
    m_playingKey.clear();
    ui->widgetBubble->setPlayingKey(QString());
}

/**
 * @brief ChatWindow::beginSendTracking
 * 记录"已发出、等回执"的消息，超时没收到回执就标红
 */
void ChatWindow::beginSendTracking(const QString &msgKey, const QJsonObject &payload)
{
    if (msgKey.isEmpty()) return;

    m_pendingPayloads.insert(msgKey, payload);
    m_pendingDeadline.insert(msgKey, QDateTime::currentMSecsSinceEpoch() + kAckTimeoutMs);
    if (m_ackTimer && !m_ackTimer->isActive()) m_ackTimer->start(1000);
}

/**
 * @brief ChatWindow::protoOfPayload
 * 同一条消息在私聊下走不同协议：文件走 SendFile、表情走 SendFace，其余走 SendMsg
 */
quint8 ChatWindow::protoOfPayload(const QJsonObject &payload)
{
    if (getIsGroup()) return SendGroupMsg;

    const int type = payload.value("type").toInt();
    if (Files == type) return SendFile;
    if (Face  == type) return SendFace;
    return SendMsg;
}

/**
 * @brief ChatWindow::sltAckCheckTimeout
 * 每秒扫一次：超过 10 秒还没等到服务端回执的，标成发送失败
 */
void ChatWindow::sltAckCheckTimeout()
{
    if (m_pendingDeadline.isEmpty())
    {
        m_ackTimer->stop();
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int failed = 0;

    for (QHash<QString, qint64>::iterator it = m_pendingDeadline.begin();
         it != m_pendingDeadline.end(); )
    {
        if (now <= it.value())
        {
            ++it;
            continue;
        }

        const QString key = it.key();
        it = m_pendingDeadline.erase(it);
        ui->widgetBubble->setItemSendState(key, SendFailed);
        ++failed;
    }

    if (failed > 0)
    {
        qWarning() << "[发送]" << failed << "条消息超时未收到服务端回执";
        QToolTip::showText(m_btnAudio ? m_btnAudio->mapToGlobal(QPoint(0, -20)) : QCursor::pos(),
                           tr("有 %1 条消息发送失败，右键气泡可重新发送").arg(failed),
                           this, QRect(), 3000);
    }

    if (m_pendingDeadline.isEmpty()) m_ackTimer->stop();
}

/**
 * @brief ChatWindow::onSendAck
 * 服务端回执：这条消息已经被受理，气泡状态改成"已送达"
 */
bool ChatWindow::hasMsgKey(const QString &msgKey) const
{
    if (msgKey.isEmpty()) return false;
    return m_pendingPayloads.contains(msgKey) || m_pendingDeadline.contains(msgKey);
}

void ChatWindow::onSendAck(const QString &msgKey)
{
    if (msgKey.isEmpty()) return;

    m_pendingDeadline.remove(msgKey);
    m_pendingPayloads.remove(msgKey);
    ui->widgetBubble->setItemSendState(msgKey, SendDone);
}

/**
 * @brief ChatWindow::failSendByKey
 * 服务端明确拒绝转发（不是好友 / 不在群里）：立刻标红，不要等超时
 */
void ChatWindow::failSendByKey(const QString &msgKey)
{
    if (msgKey.isEmpty()) return;

    m_pendingDeadline.remove(msgKey);
    ui->widgetBubble->setItemSendState(msgKey, SendFailed);
}

/**
 * @brief ChatWindow::failPendingSends
 * 链路断开：还在等回执的消息一律标成失败，用户重连后可以右键重发
 */
void ChatWindow::failPendingSends(const QString &reason)
{
    const QList<QString> keys = m_pendingDeadline.keys();
    if (keys.isEmpty()) return;

    for (const QString &key : keys)
    {
        ui->widgetBubble->setItemSendState(key, SendFailed);
    }
    m_pendingDeadline.clear();
    if (m_ackTimer) m_ackTimer->stop();

    qWarning() << "[发送]" << reason << "，" << keys.size() << "条消息未送达";
}

/**
 * @brief ChatWindow::sltResendMsg
 * 右键"重新发送"：把原始报文再发一次（文字/图片/语音会重新加密后发出）
 */
void ChatWindow::sltResendMsg(const QString &msgKey)
{
    if (!m_pendingPayloads.contains(msgKey))
    {
        QToolTip::showText(QCursor::pos(), tr("这条消息已经送达，无需重发"), this, QRect(), 1500);
        return;
    }

    const QJsonObject payload = m_pendingPayloads.value(msgKey);
    ui->widgetBubble->setItemSendState(msgKey, SendSending);

    emit signalSendMessage(protoOfPayload(payload), QJsonValue(payload));

    m_pendingDeadline.insert(msgKey, QDateTime::currentMSecsSinceEpoch() + kAckTimeoutMs);
    if (m_ackTimer && !m_ackTimer->isActive()) m_ackTimer->start(1000);

    QToolTip::showText(QCursor::pos(), tr("已重新发送"), this, QRect(), 1200);
    qDebug() << "[发送] 重发消息" << msgKey;
}

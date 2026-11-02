#include "chatbubble.h"
#include <QDebug>

#include "comapi/global.h"
#include "comapi/myapp.h"

#include <QFileInfo>
#include <QFileIconProvider>
#include <QDesktopServices>
#include <QClipboard>
#include <QApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QDateTime>
#include <QFileDialog>
#include <QStandardPaths>
#include <QFile>

#include "iteminfo.h"

// 撤回时限：2 分钟
static const qint64 kRecallWindowMs = 2 * 60 * 1000;

/// 图片消息既可能是一个本地绝对路径，也可能是内嵌的 "b64:<base64>"
static QPixmap pixmapFromMsg(const QString &strMsg)
{
    if (strMsg.startsWith("b64:"))
    {
        const QByteArray raw = QByteArray::fromBase64(strMsg.mid(4).toLatin1());
        QPixmap pm;
        if (!pm.loadFromData(raw))
        {
            qWarning() << "[气泡] 内嵌图片解码失败，长度" << raw.size();
        }
        return pm;
    }
    return QPixmap(strMsg);
}

/// 文件消息的正文有两种形态：
///   · 自己发的 → 发送时选中的本地绝对路径
///   · 收到的   → 只有文件名（消息里不会带对方机器的路径）
/// 这里统一解析成磁盘上的真实路径：绝对路径且存在就直接用，
/// 否则落到接收目录 ./Data/RecvFiles/<文件名>（可能还没下载）。
static QString resolveFilePath(const QString &strMsg)
{
    if (strMsg.isEmpty()) return QString();

    const QFileInfo info(strMsg);
    if (info.isAbsolute() && info.exists()) return info.absoluteFilePath();

    return MyApp::m_strRecvPath + info.fileName();
}

/// 自己发出的消息：在气泡左边画送达状态（发送中的灰圈 / 失败的红叹号）
static void drawSendState(QPainter *painter, qreal cx, qreal cy, quint8 state)
{
    if (SendDone == state) return;      // 已送达就不画，避免界面上到处都是对勾

    painter->save();
    painter->setRenderHints(QPainter::HighQualityAntialiasing | QPainter::Antialiasing);

    if (SendSending == state)
    {
        painter->setPen(QPen(QColor("#9A9A9A"), 1.2));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(QPointF(cx, cy), 6, 6);
    }
    else if (SendFailed == state)
    {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor("#E64340"));
        painter->drawEllipse(QPointF(cx, cy), 7, 7);

        // 白色感叹号：一竖一点
        QPen pen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap);
        painter->setPen(pen);
        painter->drawLine(QPointF(cx, cy - 3.2), QPointF(cx, cy + 0.6));
        painter->drawPoint(QPointF(cx, cy + 3.4));
    }
    painter->restore();
}

/// 把图片消息落成临时文件，供"保存图片 / 打开图片"使用；没有可落盘内容时返回空
static QString materializeImage(const QString &strMsg, const QString &dir)
{
    if (!strMsg.startsWith("b64:")) return strMsg;

    QDir().mkpath(dir);
    const QByteArray raw = QByteArray::fromBase64(strMsg.mid(4).toLatin1());
    if (raw.isEmpty()) return QString();

    const QString path = QDir(dir).filePath(
        QString("image_%1.png").arg(QDateTime::currentMSecsSinceEpoch()));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    f.write(raw);
    f.close();
    return path;
}

BubbleList::BubbleList(QWidget *parent) : QWidget(parent)
{
    initVars();
    initWgts();
    initStgs();
    initConns();
}

BubbleList::~BubbleList()
{
    d->clear();
}

/*!
  private utility functions
*/
void BubbleList::initVars()
{
}

void BubbleList::initWgts()
{
    mainLayout = new QHBoxLayout(this);
    scrollbar = new QScrollBar(this);
    d = new BubbleListPrivate(this);
    this->setMinimumWidth(300);

    mainLayout->addWidget(d);
    mainLayout->addWidget(scrollbar);
    setLayout(mainLayout);
}

void BubbleList::initStgs()
{
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    scrollbar->setRange(0, 0);
    scrollbar->hide();
}

void BubbleList::initConns()
{
    connect(scrollbar, SIGNAL(valueChanged(int)), d, SLOT(setCurrentIndex(int)));
    connect(d, SIGNAL(sig_setMaximum(int)), this, SLOT(setMaximum(int)));
    connect(d, SIGNAL(sig_setCurrentIndex(int)), scrollbar, SLOT(setValue(int)));
    connect(d, SIGNAL(sig_itemClicked(QString)), this, SIGNAL(sig_itemClicked(QString)));
    connect(d, SIGNAL(signalDownloadFile(QString)), this, SIGNAL(signalDownloadFile(QString)));
    connect(d, SIGNAL(signalWheelUp()), this, SIGNAL(signalWheelUp()));
    connect(d, SIGNAL(signalCopyText(QString)), this, SIGNAL(signalCopyText(QString)));
    connect(d, SIGNAL(signalRecallMsg(QString)), this, SIGNAL(signalRecallMsg(QString)));
    connect(d, SIGNAL(signalDeleteMsg(QString)), this, SIGNAL(signalDeleteMsg(QString)));
    connect(d, SIGNAL(signalResendMsg(QString)), this, SIGNAL(signalResendMsg(QString)));
    connect(d, SIGNAL(signalPlayAudio(QString,QString)), this, SIGNAL(signalPlayAudio(QString,QString)));
}

void BubbleList::calcGeo()
{
}

/*!
  private slot functions
*/
void BubbleList::setMaximum(int max)
{
    if (max == 0)
    {
        scrollbar->hide();
    }
    else
    {
        scrollbar->show();
    }
    scrollbar->setRange(0, max);
}

/*!
  protected functions of PYFlashList
*/
void BubbleList::resizeEvent(QResizeEvent *)
{
}

/*!
  public interfaces of PYFlashList
*/
void BubbleList::addItem(ItemInfo *item)
{
    d->addItem(item);
}

void BubbleList::addItems(QVector<ItemInfo *> items)
{
    if (items.size() < 1)
        return;
    d->addItems(items);
}

void BubbleList::appendItems(QVector<ItemInfo *> items)
{
    if (items.size() < 1)
        return;
    d->appendItems(items);
}

void BubbleList::clear()
{
    d->clear();
}

bool BubbleList::removeItemByKey(const QString &msgKey)
{
    return d->removeItemByKey(msgKey);
}

bool BubbleList::setItemSendState(const QString &msgKey, quint8 state)
{
    return d->setItemSendState(msgKey, state);
}

void BubbleList::setPlayingKey(const QString &msgKey)
{
    d->setPlayingKey(msgKey);
}

void BubbleList::render()
{
    d->render();
}

void BubbleList::setChatFont(const QFont &font)
{
    d->setChatFont(font);
}

QFont BubbleList::chatFont() const
{
    return d->chatFont();
}

/*************************************/
/*!   BubbleListPrivate section    !*/
/*************************************/
BubbleListPrivate::BubbleListPrivate(QWidget *parent) : QWidget(parent)
{
    initVars();
    initSettings();
}
/*!
  private utility functions of BubbleListPrivate
 */
void BubbleListPrivate::initVars()
{
    m_currIndex = 0;
    m_selectedIndex = -1;
    m_VisibleItemCnt = 0;
    m_ItemCounter = 0;
    m_nHoverItemIndex = -1;

    m_bAllJobsDone = false;
    m_bHover = false;

    m_font = QFont("楷体", 12);

    // 右键菜单在每次点击时动态生成（见 showItemMenu），这里不用预先创建

    // 语音播放中的声波动画
    m_animTimer = new QTimer(this);
    m_animTimer->setInterval(150);
    connect(m_animTimer, &QTimer::timeout, this, [this]()
    {
        ++m_animPhase;
        update();
    });
}

void BubbleListPrivate::initSettings()
{
    setMouseTracking(true);
}

/**
 * @brief 取某个表情的动图对象（懒加载 + 全局复用）
 *
 * 表情资源是 1.gif ~ 132.gif 的动图，用 QPixmap 加载只会拿到第一帧，
 * 所以这里用 QMovie 播放。同一个下标只创建一次，多个气泡共享同一个 QMovie，
 * 帧变化时统一触发重绘。
 *
 * @return nullptr 表示资源缺失，调用方退回静态 QPixmap 渲染
 */
QMovie *BubbleListPrivate::faceMovie(int faceIndex)
{
    const int id = faceIndex + 1;          // 资源文件从 1.gif 开始
    if (m_faceMovies.contains(id))
    {
        QMovie *cached = m_faceMovies.value(id);
        // 之前被 clear() 停掉过：重新播放，避免表情定格在最后一帧
        if (cached && cached->state() != QMovie::Running) cached->start();
        return cached;                      // 命中缓存（可能是 nullptr）
    }

    const QString path = QString(":/resource/face/%1.gif").arg(id);
    QMovie *movie = new QMovie(path, QByteArray(), this);
    if (!movie->isValid())
    {
        qWarning() << "[表情] 动图资源不可用：" << path;
        delete movie;
        m_faceMovies.insert(id, nullptr);
        return nullptr;
    }

    // CacheAll：把已解码的帧留在内存里，循环播放时不再重复解码
    movie->setCacheMode(QMovie::CacheAll);
    connect(movie, &QMovie::frameChanged, this, [this]() { update(); });
    movie->start();

    m_faceMovies.insert(id, movie);
    return movie;
}

void BubbleListPrivate::calcGeo()
{
#if 0
    // 计算当前可以刷新的item个数
    qDebug() << "calc Geo" << m_currIndex;
    int nCnt = 0;
    int nHeight = 0;
    int nCurrIndex = m_currIndex;

    while (nCurrIndex > 1) {
        nHeight = m_IIVec.at(nCurrIndex)->getItemHeight();
        nHeight += ITEM_SPACE;
        if (nHeight + 20 >= this->height()) break;
        nCnt++;
        nCurrIndex--;
    }

    m_VisibleItemCnt = nCnt;
    //    m_currIndex = nCurrIndex - nCnt;
#else
    m_VisibleItemCnt = height() / (ITEM_HEIGHT + ITEM_SPACE + 10) + 1;
#endif

    int InvisibleItemCnt = m_IIVec.count() - m_VisibleItemCnt;

    if (InvisibleItemCnt >= 0)
    {
        emit sig_setMaximum(InvisibleItemCnt);
    }
}

void BubbleListPrivate::wheelUp()
{
    if (m_IIVec.isEmpty())
    {
        // 还没有任何内容，尝试让外面去加载历史（不要在这里把索引置成 -1）
        clampCurrIndex();
        emit signalWheelUp();
        return;
    }

    if (m_currIndex < m_IIVec.size() - 1) {
        m_currIndex++;
    } else {
        m_currIndex = m_IIVec.size() - 1;
        emit signalWheelUp();
    }
    clampCurrIndex();
    update();
    emit sig_setCurrentIndex(m_currIndex);
}

void BubbleListPrivate::wheelDown()
{
    if(m_currIndex > 0){
        m_currIndex --;
    }else{
        m_currIndex = 0;
    }
    clampCurrIndex();
    update();
    emit sig_setCurrentIndex(m_currIndex);
}

/**
 * @brief canRecallItem
 * 只有"自己发的 + 2 分钟内 + 带唯一标识"的消息才允许撤回
 */
static bool canRecallItem(const ItemInfo *item)
{
    if (!item || Right != item->GetOrientation()) return false;
    if (item->GetMsgKey().isEmpty()) return false;

    const qint64 ts = item->GetTimeStamp();
    if (ts <= 0) return false;

    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - ts;
    return elapsed >= 0 && elapsed <= kRecallWindowMs;
}

/**
 * @brief BubbleListPrivate::showItemMenu
 * 所有气泡统一的右键菜单：复制 / 保存 / 撤回 / 删除
 * 撤回只对"自己发的、2 分钟内"的消息出现
 */
void BubbleListPrivate::showItemMenu(int nIndex, const QPoint &globalPos)
{
    if (nIndex < 0 || nIndex >= m_IIVec.size()) return;

    ItemInfo *item = m_IIVec.at(nIndex);
    const quint8 nType = item->GetMsgType();

    // 菜单是模态的，弹出期间这条消息可能被对端撤回（ItemInfo 会被 delete），
    // 所以先把要用到的内容拷出来，后面一律不再碰 item
    const QString strText = item->GetText();
    const QString strKey = item->GetMsgKey();
    const bool recallable = canRecallItem(item);
    const quint8 sendState = item->GetSendState();

    QMenu menu(this);
    if (Text == nType)
    {
        menu.addAction(tr("复制"));
    }
    else if (Picture == nType)
    {
        menu.addAction(tr("保存图片"));
        menu.addAction(tr("复制图片"));
    }
    else if (Files == nType)
    {
        menu.addAction(tr("下载文件"));
        menu.addAction(tr("打开文件所在目录"));
    }
    else if (Audio == nType)
    {
        menu.addAction(tr("播放语音"));
    }

    if (recallable)
    {
        menu.addAction(tr("撤回"));
    }
    if (SendFailed == sendState)
    {
        menu.addAction(tr("重新发送"));
    }
    menu.addAction(tr("删除"));

    QAction *action = menu.exec(globalPos);
    if (!action) return;

    const QString strAction = action->text();

    // ---- 复制文字 ----
    if (strAction == tr("复制"))
    {
        emit signalCopyText(strText);
        return;
    }

    // ---- 图片 ----
    if (strAction == tr("复制图片"))
    {
        const QPixmap pm = pixmapFromMsg(strText);
        if (!pm.isNull())
        {
            QApplication::clipboard()->setPixmap(pm);
        }
        return;
    }
    if (strAction == tr("保存图片"))
    {
        QString localPath = strText;
        if (localPath.startsWith("b64:"))
        {
            localPath = materializeImage(localPath, QDir::tempPath() + "/MyChat/img");
            if (localPath.isEmpty())
            {
                qWarning() << "[气泡] 图片数据无法还原";
                return;
            }
        }

        const QString fileName = QFileDialog::getSaveFileName(
            this, tr("保存图片"),
            QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation))
                .filePath(QFileInfo(localPath).fileName()),
            tr("图片文件 (*.png *.jpg *.jpeg *.bmp *.gif)"));
        if (fileName.isEmpty()) return;

        if (QFile::exists(fileName)) QFile::remove(fileName);
        if (!QFile::copy(localPath, fileName))
        {
            qWarning() << "[气泡] 保存图片失败" << localPath << "->" << fileName;
        }
        return;
    }

    // ---- 文件 ----
    if (strAction == tr("下载文件") || strAction == tr("打开文件所在目录"))
    {
        // 正文对自己发的是绝对路径，对接收方只是文件名，
        // 所以一律先解析成"磁盘上应该在的位置"，再判断下没下载。
        const QString strLocal = resolveFilePath(strText);
        if (strLocal.isEmpty()) return;
        const QFileInfo fileInfo(strLocal);

        if (strAction == tr("下载文件"))
        {
            if (fileInfo.exists())
            {
                QDesktopServices::openUrl(QUrl::fromLocalFile(strLocal));
            }
            else
            {
                // 本地没有（或已删除）就直接向服务器要，落地在 ./Data/RecvFiles/
                emit signalDownloadFile(fileInfo.fileName());
            }
        }
        else
        {
            // "打开文件所在目录"：已下载就定位到接收目录，
            // 还没下载也一样——目录不存在时先建出来，否则资源管理器会报错。
            const QString strDir = fileInfo.absolutePath();
            if (!QDir().mkpath(strDir))
            {
                qWarning() << "[气泡] 接收目录创建失败" << strDir;
                return;
            }
            QDesktopServices::openUrl(QUrl::fromLocalFile(strDir));
        }
        return;
    }

    // ---- 语音 ----
    if (strAction == tr("播放语音"))
    {
        emit signalPlayAudio(strKey, strText);
        return;
    }

    // ---- 撤回 / 删除 ----
    if (strAction == tr("撤回"))
    {
        emit signalRecallMsg(strKey);
        return;
    }
    if (strAction == tr("重新发送"))
    {
        emit signalResendMsg(strKey);
        return;
    }
    if (strAction == tr("删除"))
    {
        emit signalDeleteMsg(strKey);
        return;
    }
}

/**
 * @brief BubbleListPrivate::removeItemByKey
 * 撤回 / 删除后把气泡从界面上摘掉，同时释放 ItemInfo
 */
bool BubbleListPrivate::removeItemByKey(const QString &msgKey)
{
    if (msgKey.isEmpty()) return false;

    for (int i = 0; i < m_IIVec.size(); ++i)
    {
        if (m_IIVec.at(i)->GetMsgKey() != msgKey) continue;

        if (m_strPlayingKey == msgKey) m_strPlayingKey.clear();

        delete m_IIVec.takeAt(i);
        if (m_selectedIndex >= i) m_selectedIndex--;
        clampCurrIndex();
        calcGeo();
        update();

        // 列表空了要让外面把滚动条最大值归零，否则会留着一段空白
        emit sig_setMaximum(qMax(0, m_IIVec.count() - m_VisibleItemCnt));
        return true;
    }
    return false;
}

/**
 * @brief BubbleListPrivate::setItemSendState
 * 改一条消息的送达状态（发送中 / 已送达 / 发送失败）
 */
void BubbleListPrivate::setPlayingKey(const QString &msgKey)
{
    m_strPlayingKey = msgKey;

    if (msgKey.isEmpty())
    {
        m_animPhase = 0;
        if (m_animTimer) m_animTimer->stop();
    }
    else if (m_animTimer)
    {
        m_animTimer->start();
    }
    update();
}

bool BubbleListPrivate::setItemSendState(const QString &msgKey, quint8 state)
{
    if (msgKey.isEmpty()) return false;

    for (int i = 0; i < m_IIVec.size(); ++i)
    {
        if (m_IIVec.at(i)->GetMsgKey() != msgKey) continue;
        m_IIVec.at(i)->SetSendState(state);
        update();
        return true;
    }
    return false;
}

/*!
  protected functions of BubbleListPrivate
*/
void BubbleListPrivate::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHints(QPainter::HighQualityAntialiasing | QPainter::Antialiasing);
    drawBackground(&painter);
    drawItems(&painter);
}

void BubbleListPrivate::mouseMoveEvent(QMouseEvent *e)
{
    clampCurrIndex();
    int nItemY = ITEM_SPACE;
    for (int nIndex = m_currIndex; nIndex < m_IIVec.count(); nIndex++)
    {
        if (nItemY > this->height())
        {
            break;
        }

        int nY = this->height() - nItemY;
        QRectF bubbleRect = m_IIVec.at(nIndex)->GetBobbleRect();
        if ((e->pos().y() < (nY) && (e->pos().y() > (nY - bubbleRect.height()))) &&
                ((e->pos().x() > bubbleRect.x()) &&
                 (e->pos().x() < (bubbleRect.x() + bubbleRect.width()))))
        {
            quint8 nType = m_IIVec.at(nIndex)->GetMsgType();

            // 如果是图片或文件，可以直接打开
            if (Picture == nType || Files == nType || Audio == nType)
            {
                this->setCursor(Qt::PointingHandCursor);
                m_bHover = true;
                m_nHoverItemIndex = nIndex;
                update();
                return;
            }
        }

        // 如果有改变，需要改变回来
        if (m_bHover)
        {
            m_bHover = false;
            m_nHoverItemIndex = -1;
            update();
            return;
        }
        nItemY += bubbleRect.height() + ITEM_SPACE;
    }

    this->setCursor(Qt::ArrowCursor);
}

/**
 * @brief BubbleListPrivate::mousePressEvent
 * 鼠标右键按下
 * @param e
 */
void BubbleListPrivate::mousePressEvent(QMouseEvent *e)
{
    clampCurrIndex();
    if (Qt::LeftButton == e->button())
    {
        int nItemY = ITEM_SPACE;
        for (int nIndex = m_currIndex; nIndex < m_IIVec.count(); nIndex++)
        {
            if (nItemY > this->height()) break;

            int nY = this->height() - nItemY;
            QRectF bubbleRect = m_IIVec.at(nIndex)->GetBobbleRect();
            if ((e->pos().y() < (nY) && (e->pos().y() > (nY - bubbleRect.height()))) &&
                    ((e->pos().x() > bubbleRect.x()) &&
                     (e->pos().x() < (bubbleRect.x() + bubbleRect.width()))))
            {
                // 语音：点一下就播放
                if (Audio == m_IIVec.at(nIndex)->GetMsgType())
                {
                    emit signalPlayAudio(m_IIVec.at(nIndex)->GetMsgKey(),
                                         m_IIVec.at(nIndex)->GetText());
                }
                return;
            }
            nItemY += bubbleRect.height() + ITEM_SPACE;
        }
        return;
    }

    if (Qt::RightButton == e->button())
    {
        int nItemY = ITEM_SPACE;
        for (int nIndex = m_currIndex; nIndex < m_IIVec.count(); nIndex++)
        {
            if (nItemY > this->height())
            {
                break;
            }

            int nY = this->height() - nItemY;
            QRectF bubbleRect = m_IIVec.at(nIndex)->GetBobbleRect();
            if ((e->pos().y() < (nY) && (e->pos().y() > (nY - bubbleRect.height()))) &&
                    ((e->pos().x() > bubbleRect.x()) &&
                     (e->pos().x() < (bubbleRect.x() + bubbleRect.width()))))
            {
                // 当前选中item
                m_selectedIndex = nIndex;
                showItemMenu(nIndex, e->globalPos());
                return;
            }
            nItemY += bubbleRect.height() + ITEM_SPACE;
        }
    }
}

/**
 * @brief BubbleListPrivate::mouseDoubleClickEvent
 * 鼠标双击item
 * @param e
 */
void BubbleListPrivate::mouseDoubleClickEvent(QMouseEvent *e)
{
    clampCurrIndex();
    int nItemY = ITEM_SPACE;
    for (int nIndex = m_currIndex; nIndex < m_IIVec.count(); nIndex++)
    {
        if (nItemY > this->height())
        {
            break;
        }

        int nY = this->height() - nItemY;
        QRectF bubbleRect = m_IIVec.at(nIndex)->GetBobbleRect();
        if ((e->pos().y() < (nY) && (e->pos().y() > (nY - bubbleRect.height()))) &&
                ((e->pos().x() > bubbleRect.x()) &&
                 (e->pos().x() < (bubbleRect.x() + bubbleRect.width()))))
        {
            // 如果是图片或文件，可以直接打开
            if (Picture == m_IIVec.at(nIndex)->GetMsgType() /*|| Files == m_IIVec.at(nIndex)->GetMsgType()*/)
            {
                QString strFile = m_IIVec.at(nIndex)->GetText();
                if (strFile.startsWith("b64:"))
                {
                    strFile = materializeImage(strFile, QDir::tempPath() + "/MyChat/img");
                }
                // 如果文件存在，可以打开
                if (!strFile.isEmpty() && QFile::exists(strFile))
                {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(strFile));
                }
                return;
            }

            Q_EMIT sig_itemClicked(m_IIVec.at(nIndex)->GetText());
            break;
        }
        nItemY += bubbleRect.height() + ITEM_SPACE;
    }
}

// resize
void BubbleListPrivate::resizeEvent(QResizeEvent *)
{
    calcGeo();
}

void BubbleListPrivate::leaveEvent(QEvent *)
{
    m_HoverRect = QRectF();

    //    update();
}
void BubbleListPrivate::showEvent(QShowEvent *)
{
    calcGeo();
}

void BubbleListPrivate::wheelEvent(QWheelEvent *e)
{
    if (e->delta() > 0)
    {
        wheelUp();
    }
    else
    {
        wheelDown();
    }
}

// 绘制背景
void BubbleListPrivate::drawBackground(QPainter *painter)
{
    painter->save();
    painter->setPen(Qt::NoPen);
#if 0
    QLinearGradient BgGradient(QPoint(0,0),QPoint(0,height()));
    BgGradient.setColorAt(0.0, QColor("#EDF5E2"));
    BgGradient.setColorAt(1.0, QColor("#C9E6D8"));
    painter->setBrush(BgGradient);
#else
    painter->setBrush(QBrush(QColor("#ECECEC")));
#endif
    painter->drawRect(rect());
    painter->restore();
}

// 绘制ITEM
void BubbleListPrivate::drawItems(QPainter *painter)
{
    if (m_IIVec.count() == 0)
    {
        return;
    }

    clampCurrIndex();

    // 绘制item区域
    painter->save();

    qreal nItemY = 0;
    int nWidth = this->width();
    nWidth = (0 == nWidth % 2) ? nWidth : nWidth + 1;

    // 起始位置抬高
    nItemY = ITEM_SPACE + ITEM_TITLE_HEIGHT;

    for (int nIndex = m_currIndex; nIndex < m_IIVec.count(); nIndex++)
    {
        if (nItemY > this->height())
        {
            break;
        }

        // 解析消息内容
        QString strMsg = m_IIVec.at(nIndex)->GetText();
        quint8 msgType = m_IIVec.at(nIndex)->GetMsgType();
        int faceIndex = m_IIVec.at(nIndex)->getFace();
        //        qDebug() << "faceIndex = " << faceIndex;
        quint8 nOrientation = m_IIVec.at(nIndex)->GetOrientation();
        QString strPixmap = m_IIVec.at(nIndex)->GetStrPixmap();

        qreal bubbleWidth = 0;
        qreal bubbleHeight = 0;
        // 计算右边的宽度x
        int nX = 0;
        int nY = 0;

        QPixmap pixmap;
        QMovie *faceMoviePtr = nullptr;   // 表情动图（非空时优先于 pixmap 绘制）
        QPainterPath path;
        // 气泡和内容的矩形框
        QRectF msgRect;
        QRectF bobbleRect;

        // --------------------计算气泡和文字的绘制范围 ----------------------------//
        switch (msgType)
        {
        case Text:
        {
            // 计算文字的宽度
            QFontMetrics fm(m_font);

            bubbleWidth = fm.width(strMsg);
            bubbleHeight = fm.height();

            QStringList strMsgList = strMsg.split("\n");
            qreal nLineWidth = 0;
            qreal nLineHeight = 0;

            // 换行符会影响计算宽度和高度
            for (int i = 0; i < strMsgList.size(); i++)
            {
                if (nLineWidth < fm.width(strMsgList.at(i)))
                {
                    nLineWidth = fm.width(strMsgList.at(i));
                    nLineWidth = nLineWidth < (nWidth * 2 / 3) ? nLineWidth : (nWidth * 2 / 3);
                }
                nLineHeight = nLineWidth < (nWidth * 2 / 3) ? fm.height() * 1.5 : (((nLineWidth / ((nWidth * 2 / 3))) + 1) * ITEM_HEIGHT / 3);
                bubbleHeight += nLineHeight;
            }

            bubbleWidth = nLineWidth;
#if 0
            //            bubbleHeight = bubbleWidth < (nWidth * 2 / 3) ? ITEM_HEIGHT : (((bubbleWidth / ((nWidth * 2 / 3))) + 1) * ITEM_HEIGHT / 2);
            //            bubbleWidth = bubbleWidth < (nWidth * 2 / 3) ? bubbleWidth: (nWidth * 2 / 3);
            //            bubbleWidth = bubbleWidth < 30 ? 30 : bubbleWidth;
#endif

            // 文字初始化高度
            nY = this->height() - nItemY - bubbleHeight;
        }
            break;
        case Audio:
        {
            // 语音气泡宽度按时长增长：1 秒约 3px，最短 70，最长 220
            const int dur = m_IIVec.at(nIndex)->GetDuration();
            bubbleWidth = qBound(70.0, 70.0 + dur / 1000.0 * 3.0, 220.0);
            bubbleHeight = 34;

            nY = this->height() - nItemY - bubbleHeight;
        }
            break;
        case Picture:
        {
            pixmap = pixmapFromMsg(strMsg);
            if (pixmap.isNull())
            {
                pixmap = QPixmap(":/resource/background/ic_picture.png");
            }
            // 图片过大限制
            if (pixmap.width() > 200 || pixmap.height() > 100)
            {
                pixmap = pixmap.scaled(200, 100, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }

            bubbleWidth = pixmap.width();
            bubbleHeight = pixmap.height() + 10;

            // 文字初始化高度
            nY = this->height() - nItemY - bubbleHeight;
        }
            break;
        case  Face:
        {
            // 表情按 faceIndex 取资源（1.gif ~ 132.gif）：发送方和接收方都用同一个下标，
            // 不能用 strMsg —— 老版本把下标写进 Text，格式不统一会画错。
            faceMoviePtr = faceMovie(faceIndex);
            if (faceMoviePtr)
            {
                // 用当前帧的尺寸算气泡；首帧还没解码时退回 frameRect
                QSize sz = faceMoviePtr->currentPixmap().size();
                if (sz.isEmpty()) sz = faceMoviePtr->frameRect().size();
                bubbleWidth  = sz.width();
                bubbleHeight = sz.height() + 10;
            }
            else
            {
                pixmap = QPixmap(QString(":/resource/face/%1.gif").arg(faceIndex + 1));
                bubbleWidth  = pixmap.width();
                bubbleHeight = pixmap.height() + 10;
            }
            // 文字初始化高度
            nY = this->height() - nItemY - bubbleHeight;

            break;
        }
        case Files:
        {
            // 正文可能只是文件名，必须解析到接收目录再判断是否已下载
            QFileInfo fileInfo(resolveFilePath(strMsg));
            QString strSize = m_IIVec.at(nIndex)->GetFileSizeString();

            // 文件图标

            pixmap = QPixmap(":/resource/images/ic_zip.png");

            // 先判断文件是否存在
            if (fileInfo.exists())
            {
                strSize = "文件大小: ";
                strSize += myHelper::CalcSize(fileInfo.size());
#ifdef Q_OS_WIN32
                QFileIconProvider provider;
                QIcon icon = provider.icon(fileInfo);
                pixmap = icon.pixmap(48, 48);
#endif
            }
            else
            {
                strSize = "文件未下载";
            }

            // 计算文字的宽度
            QFont font("微软雅黑", 10);
            QFontMetrics fm(font);
            // 显示文件名，路径不显示
            strMsg = fileInfo.fileName();
            // 计算气泡的size
            if (fm.width(strMsg) < fm.width(strSize))
            {
                bubbleWidth = fm.width(strSize) + pixmap.width() + 20;
            }
            else
            {
                bubbleWidth = fm.width(strMsg) + pixmap.width() + 20;
            }

            bubbleHeight = 60;

            bubbleWidth = bubbleWidth < (nWidth * 2 / 3) ? bubbleWidth : (nWidth * 2 / 3);
            bubbleWidth = bubbleWidth < 60 ? 60 : bubbleWidth;

            strMsg += "\n";
            strMsg += strSize;

            // 文字初始化高度
            nY = this->height() - nItemY - bubbleHeight;
        }
            break;
        default:
            break;
        }

        // --------------------绘制气泡和文字 ----------------------------//
        if (Right == nOrientation)
        {
            painter->save();
            painter->setPen(QPen(QColor("#D3D3D3"), 1));
            // 绘制边框
            painter->drawRoundedRect(nWidth - 55, nY, 50, 50, 2, 2);
            // 绘制头像
            QPixmap pixmap(strPixmap);
            if (!pixmap.isNull())
            {
                painter->drawPixmap(nWidth - 55 + 1, nY + 1, 48, 48, pixmap);
            }
            painter->restore();

            // 绘制名字和当前时间
            painter->save();
            // 计算文字的宽度
            QFont font("微软雅黑", 8);
            QFontMetrics fm(font);
            QString strTitle = m_IIVec.at(nIndex)->GetDatetime() + "  ";
            strTitle += m_IIVec.at(nIndex)->GetName();

            painter->setPen(QColor("#666666"));
            painter->setFont(font);
            painter->drawText(nWidth - 55 - fm.width(strTitle) - 10, nY + fm.height() / 2, strTitle);
            painter->restore();

            // 根据消息内容计算气泡区域
            nX = (nWidth)-95 - bubbleWidth;
            if (nX < 0)
            {
                nX = -bubbleWidth - 95 + nWidth;
            }

            // 计算气泡框
            nY += ITEM_TITLE_HEIGHT;
            bobbleRect = QRectF(nX, nY, bubbleWidth + 20, bubbleHeight);
            msgRect = QRectF(nX + 10, nY, bubbleWidth, bubbleHeight);

            painter->save();
            // 计算气泡右边的三角
            path.addRoundedRect(bobbleRect, 10, 10);
            path.moveTo(nWidth - 75, nY + 15);
            path.lineTo(nWidth - 65, nY + 20);
            path.lineTo(nWidth - 75, nY + 25);
            //            painter->fillPath(path, itemGradient);
            painter->setPen(Qt::red);
            painter->fillPath(path, QColor("#FFFFFF"));
            painter->restore();
        }
        else
        {
            // 绘制头像
            painter->save();
            painter->setPen(QColor("#D3D3D3"));
            // 绘制边框
            painter->drawRoundedRect(5, nY, 50, 50, 2, 2);

            // 绘制头像
            QPixmap pixmap(strPixmap);
            if (!pixmap.isNull())
            {
                painter->drawPixmap(6, nY + 1, 48, 48, pixmap);
            }

            painter->restore();

            nX = 75;
            // 绘制名字和当前时间
            painter->save();
            // 计算文字的宽度
            QFont font("微软雅黑", 8);
            QFontMetrics fm(font);

            QString strTitle = m_IIVec.at(nIndex)->GetName() + " ";
            strTitle += m_IIVec.at(nIndex)->GetDatetime();

            painter->setPen(QColor("#666666"));
            painter->setFont(font);
            painter->drawText(nX, nY + fm.height() / 2, strTitle);
            painter->restore();

            // 绘制气泡区域
            painter->save();
            nY += ITEM_TITLE_HEIGHT;
            bobbleRect = QRectF(nX, nY, bubbleWidth + 20, bubbleHeight);
            // 绘制文字区域
            msgRect = QRectF(nX + 10, nY, bubbleWidth, bubbleHeight);

            // 左边三角
            path.addRoundedRect(bobbleRect, 5, 5);
            path.moveTo(nX, nY + 15);
            path.lineTo(nX - 10, nY + 20);
            path.lineTo(nX, nY + 25);
            // 背景色
            painter->fillPath(path, QColor("#9EE656"));
            painter->restore();
        }

        // 文字颜色：发送方自定义过的用自定义色，否则用默认黑
        const QString strColor = m_IIVec.at(nIndex)->GetTextColor();
        painter->setPen(strColor.isEmpty() ? QColor("#000000") : QColor(strColor));
        painter->setFont(m_font);

        if (Text == msgType)
        {
            painter->drawText(msgRect, strMsg, Qt::AlignVCenter | Qt::AlignLeft);
        }
        else if (Picture == msgType)
        {
            painter->drawPixmap(nX + 10, nY + 5, pixmap.width(), pixmap.height(), pixmap);
        }
        else if (Face == msgType)
        {
            // 动图：每帧都是从 QMovie 取当前帧；静态回退时用 pixmap
            if (faceMoviePtr)
            {
                const QPixmap frame = faceMoviePtr->currentPixmap();
                if (!frame.isNull())
                {
                    painter->drawPixmap(nX + 10, nY + 5, frame.width(), frame.height(), frame);
                }
            }
            else
            {
                painter->drawPixmap(nX + 10, nY + 5, pixmap.width(), pixmap.height(), pixmap);
            }
        }
        else if (Audio == msgType)
        {
            const bool playing = (m_strPlayingKey == m_IIVec.at(nIndex)->GetMsgKey());

            // 麦克风图标
            QPixmap ico(":/resource/common/ic_record.png");
            if (!ico.isNull())
            {
                ico = ico.scaled(18, 18, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                painter->drawPixmap(nX + 12, nY + (bubbleHeight - ico.height()) / 2,
                                    ico.width(), ico.height(), ico);
            }

            // 时长：3" 这样
            const int sec = qMax(1, m_IIVec.at(nIndex)->GetDuration() / 1000);
            painter->save();
            painter->setPen(QColor(playing ? "#12B7F5" : "#333333"));
            QFont font("微软雅黑", 10);
            painter->setFont(font);
            painter->drawText(QRectF(nX + 36, nY, bubbleWidth - 24, bubbleHeight),
                              QString("%1\"").arg(sec),
                              Qt::AlignVCenter | Qt::AlignLeft);
            painter->restore();

            // 播放中：右侧跟着几根跳动的"声波"竖条
            if (playing)
            {
                painter->save();
                painter->setPen(Qt::NoPen);
                painter->setBrush(QColor("#12B7F5"));
                for (int i = 0; i < 3; ++i)
                {
                    const int h = 6 + ((m_animPhase + i) % 3) * 3;
                    painter->drawRect(QRectF(nX + bubbleWidth - 18 + i * 5,
                                             nY + (bubbleHeight - h) / 2, 3, h));
                }
                painter->restore();
            }
        }
        else if (Files == msgType)
        {
            painter->drawPixmap(nX + 10, nY + 5 + (48 - pixmap.height()) / 2, pixmap.width(), pixmap.height(), pixmap);
            msgRect.setX(msgRect.x() + pixmap.width() + 15);
            painter->save();
            painter->setPen(Qt::blue);
            QFont font("微软雅黑", 10);
            font.setUnderline(m_bHover && (nIndex == m_nHoverItemIndex));
            painter->setFont(font);
            painter->drawText(msgRect, strMsg, Qt::AlignVCenter | Qt::AlignLeft);
            painter->restore();
        }

        // 自己发的消息：气泡左侧标出"发送中 / 发送失败"
        if (Right == nOrientation)
        {
            const qreal cx = bobbleRect.x() - 16;
            if (cx > 6)
            {
                drawSendState(painter, cx, bobbleRect.y() + bubbleHeight / 2,
                              m_IIVec.at(nIndex)->GetSendState());
            }
        }

        m_IIVec.at(nIndex)->SetBobbleRect(bobbleRect);
        nItemY += (bobbleRect.height()) + ITEM_SPACE + ITEM_TITLE_HEIGHT;
    }
    painter->restore();
}

/*!
  public interfaces of BubbleListPrivate
 */
void BubbleListPrivate::addItem(ItemInfo *item)
{
    m_IIVec.push_front(item);
    m_currIndex = 0;
    update();
}

/**
 * @brief BubbleListPrivate::addItems
 * 初始加载：用它替换整个列表（原来这里把"历史页数"当成条目数加到索引上，
 * 索引会跑到 size 之外，导致 QVector::at 越界断言）
 */
void BubbleListPrivate::addItems(QVector<ItemInfo *> items)
{
    // 替换整个列表：旧条目同样要释放
    qDeleteAll(m_IIVec);
    m_IIVec = items;
    m_currIndex = 0;
    clampCurrIndex();
    update();
}

/**
 * @brief BubbleListPrivate::appendItems
 * 加载更早的历史：追加到末尾，当前查看位置保持不变
 */
void BubbleListPrivate::appendItems(QVector<ItemInfo *> items)
{
    m_IIVec.append(items);
    clampCurrIndex();
    update();
}

void BubbleListPrivate::clampCurrIndex()
{
    if (m_IIVec.isEmpty())
    {
        m_currIndex = 0;
        return;
    }
    if (m_currIndex < 0)
    {
        m_currIndex = 0;
    }
    else if (m_currIndex > m_IIVec.size() - 1)
    {
        m_currIndex = m_IIVec.size() - 1;
    }
}

void BubbleListPrivate::clear()
{
    // ItemInfo 不是 QObject，容器清空前必须自己释放，否则每次重开会话都漏一批
    qDeleteAll(m_IIVec);
    m_IIVec.clear();
    m_currIndex = 0;
    m_VisibleItemCnt = 0;
    m_ItemCounter = 0;

    m_bAllJobsDone = false;

    // 列表清空了就没有表情要显示，先让动图停下来，下次真正绘制时会自动重新播放
    foreach (QMovie *movie, m_faceMovies) {
        if (movie && movie->state() != QMovie::NotRunning) movie->stop();
    }

    update();
}

void BubbleListPrivate::render()
{
    update();
}

/**
 * @brief BubbleListPrivate::setChatFont
 * 切换聊天区字体。气泡高度是按字体算出来的，所以这里要重排并重绘
 */
void BubbleListPrivate::setChatFont(const QFont &font)
{
    if (m_font == font) return;

    m_font = font;
    // 字体变了，之前保存的 hover 状态就没意义了
    m_bHover = false;
    m_nHoverItemIndex = -1;
    calcGeo();
    update();
}

/*!
  public slot functions of BubbleListPrivate
*/
void BubbleListPrivate::setCurrentIndex(int curIndex)
{
    if (curIndex == m_currIndex)
    {
        return;
    }

    m_currIndex = curIndex;
    clampCurrIndex();   // 滚动条的值可能超过条目数，必须夹紧
}

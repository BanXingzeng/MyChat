#ifndef CHATBUBBLE_H
#define CHATBUBBLE_H

#include <QWidget>
#include <QPainter>
#include <QPaintEvent>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QTimer>
#include <QMenu>
#include <QMovie>
#include <QHash>
#include "comapi/unit.h"


class ItemInfo;
class BubbleListPrivate;
class BubbleList: public QWidget
{
    Q_OBJECT
public:
    BubbleList(QWidget* parent = 0);
    ~BubbleList();

public:
    /// proxy public interfaces for PYFlashListPrivate
    void addItem(ItemInfo *item);
    void addItems(QVector<ItemInfo*> items);     // 初始加载：替换整个列表
    void appendItems(QVector<ItemInfo*> items);  // 加载更早的历史：追加到末尾

    void clear();

    // 按 msg_key 移除一条消息（撤回 / 删除后同步界面）
    bool removeItemByKey(const QString &msgKey);

    // 自己发出的消息的送达状态（发送中 / 已送达 / 发送失败）
    bool setItemSendState(const QString &msgKey, quint8 state);
    // 当前正在播放的语音（空串表示没有在播）
    void setPlayingKey(const QString &msgKey);

    void render();

    // 聊天区使用的字体（用户可通过工具栏修改），不设置则用默认楷体 12
    void setChatFont(const QFont &font);
    QFont chatFont() const;

    void setCurrItem(const int &index) {
        scrollbar->setValue(index);
    }

protected:
    QSize sizeHint() const
    {
        return QSize(SIZE_HINT);
    }

    void resizeEvent(QResizeEvent *);


private:
    /// private utility functoins
    void initVars();
    void initWgts();
    void initStgs();
    void initConns();

    void calcGeo();

private Q_SLOTS:
    void setMaximum(int max);

private:
    QHBoxLayout* mainLayout;
    QScrollBar* scrollbar;
    BubbleListPrivate* d;

Q_SIGNALS:
    void sig_setCurrentIndex(int currIndex);
    void sig_itemClicked(const QString& str);
    void signalDownloadFile(const QString &fileName);
    void signalWheelUp();
    // 右键菜单动作
    void signalCopyText(const QString &text);
    void signalRecallMsg(const QString &msgKey);
    void signalDeleteMsg(const QString &msgKey);
    void signalResendMsg(const QString &msgKey);            // 重新发送（发送失败时）
    // 点击语音气泡：请求播放
    void signalPlayAudio(const QString &msgKey, const QString &path);
};

class BubbleListPrivate : public QWidget
{
    Q_OBJECT
public:
    explicit BubbleListPrivate(QWidget *parent = 0);

public:
    /// public interfaces
    void addItem(ItemInfo *item);
    void addItems(QVector<ItemInfo*> items);     // 初始加载：替换整个列表
    void appendItems(QVector<ItemInfo*> items);  // 加载更早的历史：追加到末尾

    void setChatFont(const QFont &font);
    QFont chatFont() const { return m_font; }

    void clear();

    void render();

    int itemCount() const
    {
        return m_IIVec.count();
    }

    // 按 msg_key 摘掉一条消息
    bool removeItemByKey(const QString &msgKey);

    // 自己发出的消息的送达状态
    bool setItemSendState(const QString &msgKey, quint8 state);
    // 当前正在播放的语音（播放中会有声波动画）
    void setPlayingKey(const QString &msgKey);

public Q_SLOTS:
    void setCurrentIndex(int curIndex);

protected:
    void paintEvent(QPaintEvent *);

    void mouseMoveEvent(QMouseEvent *);

    void mousePressEvent(QMouseEvent *);

    void mouseDoubleClickEvent(QMouseEvent *e);

    void resizeEvent(QResizeEvent *);

    void leaveEvent(QEvent *);

    void showEvent(QShowEvent *);

    void wheelEvent(QWheelEvent *);

private:
    /// painting functions
    void drawBackground(QPainter* painter);

    void drawItems(QPainter* painter);

    void drawHoverRect(QPainter* painter);


private:
    /// private utility functoins
    void initVars();
    void initSettings();
    void calcGeo();
    void wheelUp();
    void wheelDown();
    void clampCurrIndex();   // 修正 m_currIndex，保证它始终落在 [0, size-1]
    void showItemMenu(int nIndex, const QPoint &globalPos);   // 统一的右键菜单

    // 表情是 gif 动图：按下标懒加载一个 QMovie，返回空表示该表情不可用（退回静态图）
    QMovie *faceMovie(int faceIndex);

private:
    // 如果消息内容多了，不方便修改！~
    QVector<ItemInfo*> m_IIVec;

    int m_currIndex;
    int m_selectedIndex;
    int m_VisibleItemCnt;
    int m_ItemCounter;

    bool m_bAllJobsDone;

    QRectF m_HoverRect;

    QString m_strHoverText;

    QFont m_font;

    bool    m_bHover;
    int     m_nHoverItemIndex;
    QString m_strPlayingKey;    // 正在播放的语音消息 key
    QTimer *m_animTimer = nullptr;  // 播放中驱动声波动画
    int     m_animPhase = 0;

    // 表情动图缓存：faceIndex -> QMovie（值为 nullptr 表示该资源不存在）
    // 同一张表情在多条消息里出现时共用同一个 QMovie，避免重复解码
    QHash<int, QMovie*> m_faceMovies;

Q_SIGNALS:
    void sig_setMaximum(int max);
    void sig_setCurrentIndex(int currIndex);
    void sig_itemClicked(const QString& str);
    void signalDownloadFile(const QString &fileName);
    void signalWheelUp();
    void signalCopyText(const QString &text);
    void signalRecallMsg(const QString &msgKey);
    void signalDeleteMsg(const QString &msgKey);
    void signalResendMsg(const QString &msgKey);
    void signalPlayAudio(const QString &msgKey, const QString &path);
};
#endif // CHATBUBBLE_H

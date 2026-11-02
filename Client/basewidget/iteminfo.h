#ifndef ITEMINFO_H
#define ITEMINFO_H

#include <QtCore/qglobal.h>
#include <QPixmap>
#include <QRectF>
#include "comapi/unit.h"

class ItemInfo
{
public:
    ItemInfo(QWidget *parent = nullptr);
    ItemInfo(const QString& strName, const QString& datetime,
             const QString &pixmap, const QString &text,
             const QString& strSize, const quint8 &orientation = Right,
             const quint8 &msgType = Text ,QWidget *parent = nullptr);

    ~ItemInfo();

public:
    // public interfaces

    void SetName(const QString &text);
    QString GetName() const;

    void SetDatetime(const QString &text);
    QString GetDatetime() const;

    void SetHeadPixmap(const QString &pixmap);
    QString GetStrPixmap() const;

    void SetText(const QString& text);
    QString GetText() const;

    void SetFace(const int index);
    int getFace() const;

    void SetFileSizeString(const QString& strSize);
    QString GetFileSizeString() const;

    // 文字消息的自定义颜色（空串表示用默认色）
    void SetTextColor(const QString &color);
    QString GetTextColor() const;

    void SetOrientation(quint8 orientation);
    quint8 GetOrientation() const;

    void SetMsgType(const quint8 &msgType);
    quint8 GetMsgType() const;

    // 消息唯一标识（收发双方一致），撤回时用来定位同一条消息
    void SetMsgKey(const QString &key);
    QString GetMsgKey() const;

    // 消息时间戳（毫秒），用于判断是否还在"2 分钟内可撤回"的窗口内
    void SetTimeStamp(qint64 ts);
    qint64 GetTimeStamp() const;

    // 语音消息时长（毫秒）
    void SetDuration(int ms);
    int  GetDuration() const;

    // 自己发出的消息的送达状态：SendSending / SendDone / SendFailed
    void SetSendState(quint8 state);
    quint8 GetSendState() const;

    void SetItemHeight(qreal itemHeight);
    qreal GetItemHeight() const;

    void SetBobbleRect(const QRectF &bobbleRect);
    QRectF GetBobbleRect() const;

private:
    // 消息的标题，比如名字和时间
    QString     m_strName;
    // 消息日期
    QString     m_strDatetime;
    // 头像图片
    QString     m_strPixmap;
    //表情序号
    int         m_faceIndex;
    // 消息内容
    QString     m_strText;
    // 如果是文件，会显示文件大小
    QString     m_strSize;
    // 文字消息颜色（#RRGGBB，空串表示默认）
    QString     m_strTextColor;

    // 气泡方向
    quint8      m_orientation;
    // 消息内容
    quint8      m_msgType;
    // 消息唯一标识
    QString     m_strMsgKey;
    // 消息时间戳（毫秒）
    qint64      m_lTimeStamp;
    // 语音消息时长（毫秒）
    int         m_nDuration;
    // 暂时没有用
    qreal       m_itemHeight;
    // 窗口的坐标，在点击查询时需要访问
    QRectF      m_bobbleRect;
    // 消息状态（发送中 / 已送达 / 发送失败）
    quint8      m_nStatus;
};


#endif // ITEMINFO_H

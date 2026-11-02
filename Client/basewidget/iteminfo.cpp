#include "iteminfo.h"

ItemInfo::ItemInfo(QWidget *parent) :
    m_strName(""),
    m_strDatetime(""),
    m_strPixmap(""),
    m_faceIndex(0),
    m_strText(""),
    m_strSize(""),
    m_strTextColor(""),
    m_orientation(Left),
    m_msgType(Text),
    m_strMsgKey(""),
    m_lTimeStamp(0),
    m_nDuration(0),
    m_itemHeight(ITEM_HEIGHT),
    m_nStatus(SendDone)
{

}

ItemInfo::ItemInfo(const QString &strName, const QString &datetime, const QString &pixmap, const QString &text, const QString &strSize, const quint8 &orientation, const quint8 &msgType, QWidget *parent) :
    m_strName(strName),
    m_strDatetime(datetime),
    m_strPixmap(pixmap),
    m_faceIndex(0),
    m_strText(text),
    m_strSize(strSize),
    m_strTextColor(""),
    m_orientation(orientation),
    m_msgType(msgType),
    m_strMsgKey(""),
    m_lTimeStamp(0),
    m_nDuration(0),
    m_itemHeight(ITEM_HEIGHT),
    m_nStatus(SendDone)
{

}

ItemInfo::~ItemInfo() {

}

void ItemInfo::SetName(const QString &text)
{
    m_strName = text;
}

QString ItemInfo::GetName() const
{
    return m_strName;
}

void ItemInfo::SetDatetime(const QString &text)
{
    m_strDatetime = text;
}

QString ItemInfo::GetDatetime() const
{
    return m_strDatetime;
}

void ItemInfo::SetHeadPixmap(const QString &pixmap)
{
    m_strPixmap = pixmap;
}

QString ItemInfo::GetStrPixmap() const {
    return m_strPixmap;
}


void ItemInfo::SetText(const QString &text)
{
    m_strText = text;
}

QString ItemInfo::GetText() const
{
    return m_strText;
}

void ItemInfo::SetFace(const int index)
{
    m_faceIndex = index;
}

int ItemInfo::getFace() const
{
    return  m_faceIndex;
}

void ItemInfo::SetFileSizeString(const QString &strSize)
{
    m_strSize = strSize;
}

QString ItemInfo::GetFileSizeString() const
{
    return m_strSize;
}

void ItemInfo::SetTextColor(const QString &color)
{
    m_strTextColor = color;
}

QString ItemInfo::GetTextColor() const
{
    return m_strTextColor;
}

quint8 ItemInfo::GetOrientation() const
{
    return m_orientation;
}

void ItemInfo::SetOrientation(quint8 orientation)
{
    m_orientation = orientation;
}

quint8 ItemInfo::GetMsgType() const
{
    return m_msgType;
}

void ItemInfo::SetMsgType(const quint8 &msgType)
{
    m_msgType = msgType;
}

qreal ItemInfo::GetItemHeight() const
{
    return m_itemHeight;
}

void ItemInfo::SetItemHeight(qreal itemHeight)
{
    m_itemHeight = itemHeight;
}

QRectF ItemInfo::GetBobbleRect() const
{
    return m_bobbleRect;
}

void ItemInfo::SetBobbleRect(const QRectF &bobbleRect)
{
    m_bobbleRect = bobbleRect;
}

void ItemInfo::SetMsgKey(const QString &key)
{
    m_strMsgKey = key;
}

QString ItemInfo::GetMsgKey() const
{
    return m_strMsgKey;
}

void ItemInfo::SetTimeStamp(qint64 ts)
{
    m_lTimeStamp = ts;
}

qint64 ItemInfo::GetTimeStamp() const
{
    return m_lTimeStamp;
}

void ItemInfo::SetDuration(int ms)
{
    m_nDuration = ms;
}

int ItemInfo::GetDuration() const
{
    return m_nDuration;
}

void ItemInfo::SetSendState(quint8 state)
{
    m_nStatus = state;
}

quint8 ItemInfo::GetSendState() const
{
    return m_nStatus;
}

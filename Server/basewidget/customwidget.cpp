#include "customwidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QStyleOption>
#include <QDebug>

CustomWidget::CustomWidget(QWidget *parent) : QWidget(parent)
{

}

void CustomWidget::paintEvent(QPaintEvent* event)
{
    QStyleOption opt;
    opt.init(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

CustomMoveWidget::CustomMoveWidget(QWidget *parent) : CustomWidget(parent)
{
    m_isPressed = false;
}
CustomMoveWidget::~CustomMoveWidget()
{

}

void CustomMoveWidget::mousePressEvent(QMouseEvent* event)
{
    if(event->button() == Qt::LeftButton)
    {
        m_isPressed = true;
        mousePoint = event->pos();
        return;
    }
    return QWidget::mousePressEvent(event);

}
void CustomMoveWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if(event->button() == Qt::LeftButton)
    {
        m_isPressed = false;
        return;
    }
    return QWidget::mouseReleaseEvent(event);
}
void CustomMoveWidget::mouseMoveEvent(QMouseEvent* event)
{
    if(m_isPressed && (event->buttons() & Qt::LeftButton)){
        QPoint movePoint = event->globalPos() - mousePoint;
        this->move(movePoint);
        return;
    }
    return QWidget::mouseMoveEvent(event);
}

void CustomMoveWidget::setQss(const QString &filePath)
{
    QFile file(filePath);
    if(!file.open(QIODevice::ReadOnly)){
        qDebug() << "Qss文件打开失败:" << filePath;
        return;
    }
    // 注意：样式表文件是 UTF-8 编码，必须按 UTF-8 显式解码。
    // 若用 QTextStream 默认编码（Windows 简体中文下为 GBK），中文注释会被错误解码，
    // 注释结束符 "*/" 中的 '*' 会被当成双字节字符的一部分吃掉，导致注释无法闭合，
    // 后面大段的选择器被整体当成注释，表现就是 QSS "不生效"。
    setStyleSheet(QString::fromUtf8(file.readAll()));
    file.close();
}

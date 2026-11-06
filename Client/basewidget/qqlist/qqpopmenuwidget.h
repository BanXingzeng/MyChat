#ifndef QQPOPMENUWIDGET_H
#define QQPOPMENUWIDGET_H

#include <QMenu>
#include <QMouseEvent>
#include <basewidget/customwidget.h>

/**
 * @brief 检测鼠标右键，实现弹出菜单功能
 */
class QQPopMenuWidget : public CustomWidget
{
    Q_OBJECT
public:
    explicit QQPopMenuWidget(QWidget *parent = nullptr);

signals:
    void onpopmenuwillshow(QQPopMenuWidget * widget, QMenu *menu);

public slots:

public:
    void setPopMenu(QMenu *menu);

protected slots:

protected:
    QMenu *popMenu = nullptr;

public:
    virtual void mousePressEvent(QMouseEvent *event);
};

#endif // QQPOPMENUWIDGET_H

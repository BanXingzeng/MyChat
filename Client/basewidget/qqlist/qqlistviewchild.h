#ifndef QQLISTVIEWCHILD_H
#define QQLISTVIEWCHILD_H

#include <QWidget>
#include <QMouseEvent>

#include "qqcell.h"
#include "qqpopmenuwidget.h"

/**
            View视图
 * @brief 在QQListViewChild的鼠标右键点击事件中，
 * 根据绑定的QQCell的类型（组 / 子项 / 扩展组）动态创建不同的QMenu菜单对象
 * ，通过父类QQPopMenuWidget的setPopMenu绑定并触发弹出，
 * 同时发射signalRightClicked信号传递当前QQCell数据。
 */
class QQListViewChild : public QQPopMenuWidget
{
    Q_OBJECT

public:
    explicit QQListViewChild(QQPopMenuWidget *parent = nullptr);
    ~QQListViewChild();

signals:
    void onChildDidSelected(QQListViewChild *child);
    void onChildDidDoubleClicked(QQCell *cell);
    void signalRightClicked(QQCell *cell);
protected:
    void mousePressEvent(QMouseEvent *ev);
    void mouseDoubleClickEvent(QMouseEvent * event);
    void enterEvent(QEvent *event);
    void leaveEvent(QEvent *event);

    void paintEvent(QPaintEvent *);
public:
    QQCell *cell = nullptr;
private:
    bool m_bEntered;

private:
};

#endif // QQLISTVIEWCHILD_H

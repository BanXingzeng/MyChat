#ifndef QQLISTVIEWGROUP_H
#define QQLISTVIEWGROUP_H

#include <QWidget>
#include <QMenu>
#include <QMouseEvent>

#include "ui_qqlistviewgroup.h"
#include "qqcell.h"
#include "qqpopmenuwidget.h"

namespace Ui {
class QQListViewGroup;
}
/**
            View视图
 * @brief 在鼠标右键点击事件中依据绑定的QQCell组类型创建专属弹出菜单，
 * 通过setPopMenu绑定并触发展示，同时可响应组展开状态变更信号。
 */
class QQListViewGroup : public QQPopMenuWidget
{
    Q_OBJECT

public:
    explicit QQListViewGroup(QQPopMenuWidget *parent = 0);
    ~QQListViewGroup();

public:
    void setQQCell(QQCell *c);

signals:
    void onGroupOpenStatusDidChanged(QQListViewGroup *group);

protected:
    void mousePressEvent(QMouseEvent *ev);

public:
    Ui::QQListViewGroup *ui;

private:
    QQCell *cell = nullptr;
};

#endif // QQLISTVIEWGROUP_H

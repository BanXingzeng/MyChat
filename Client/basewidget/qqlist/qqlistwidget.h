#ifndef QQLISTWIDGET_H
#define QQLISTWIDGET_H

#include <QListWidget>
#include <QList>
#include <QHash>
#include <QMenu>

#include "qqcell.h"
#include "qqlistviewgroup.h"
#include "qqlistviewchild.h"

/**
            Controller控制器
 * @brief 管理所有QQCell数据，为组/子项分别绑定mGroupMenu/mChildMenu专属弹出菜单，
 * 监听子控件右键点击信号后，根据 QQCell 类型触发对应菜单展示，并发射菜单即将显示信号，
 * 同时处理组展开状态变更、子项选中/双击等交互逻辑
 */
class QQListWidget : public QListWidget
{
    Q_OBJECT
public:
    explicit QQListWidget(QWidget *parent = nullptr);

    QQCell *GetRightClickedCell();
signals:
    void on_popmenu_will_show(QQListWidget *listWidget,QQPopMenuWidget *widget, QMenu *menu);
    void onChildDidDoubleClicked(QQCell *child);

public slots:
    void onGroupOpenDidChanged(QQListViewGroup *group);

    void onChildDidSelected(QQListViewChild *child);

    void on_popmenuWillShow(QQPopMenuWidget *widget, QMenu *menu);

    void SltCellRightCicked(QQCell *cell);

private:
    QList<QQCell *> cells;                      //所有的数据
    QMenu *mGroupMenu = nullptr;
    QMenu *mChildMenu = nullptr;

    QQCell *m_nRightClickCell;
public:
    void insertQQCell(QQCell * cell);
    void removeQQCell(QQCell * cell);

    void clearAllCells();
    void clearListWidgetItems();

    void upload();
    QQCell *getGroupForName(QString *groupName);

    void setGroupPopMenu(QMenu *menu);
    void setChildPopMenu(QMenu *menu);

    QList<QQCell *> getCells() const;

private:
    void setBackGroundColor(QWidget *widget,int index);

    void AddChildItem(QQCell *cell);
};

#endif // QQLISTWIDGET_H

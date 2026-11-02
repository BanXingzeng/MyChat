#include "qqlistwidget.h"
#include "comapi/unit.h"

#include <QDebug>

QQListWidget::QQListWidget(QWidget *parent) :
    QListWidget(parent)
{
    m_nRightClickCell = nullptr;
}

QQCell *QQListWidget::GetRightClickedCell()
{
    return m_nRightClickCell;
}

void QQListWidget::setGroupPopMenu(QMenu *menu)
{
    mGroupMenu = menu;
}

void QQListWidget::setChildPopMenu(QMenu *menu)
{
    mChildMenu = menu;
}

void QQListWidget::insertQQCell(QQCell * cell)
{
    if (cell->type == QQCellType_Group ||
            cell->type == QQCellType_GroupEx)
    {
        cells.append(cell);
    }
    else
    {
        foreach (QQCell *group, cells)
        {
            if (!group->groupName.compare(cell->groupName))
            {
                group->childs.append(cell);

                break;
            }
        }
    }

    upload();
}

void QQListWidget::removeQQCell(QQCell * cell)
{
    if (cell->type == QQCellType_Group ||
            cell->type == QQCellType_GroupEx)
    {
        cells.removeOne(cell);
    }
    else
    {
        for (QQCell *group : cells)
        {
            if (!group->groupName.compare(cell->groupName))
            {
                group->childs.removeOne(cell);

                break;
            }
        }
    }

    upload();
}

void QQListWidget::clearAllCells()
{
    for(QQCell* groupCell : cells){
        if(groupCell != nullptr){
            for(QQCell* childCell : groupCell->childs){
                if(childCell != nullptr){
                    delete childCell;
                    childCell = nullptr;
                }
            }
            groupCell->childs.clear(); // 清空子项列表，避免野指针

            delete groupCell;
            groupCell = nullptr;
        }
    }

    cells.clear();  //清空数据，防止访问野指针
    m_nRightClickCell = nullptr;    // 右键选中的 cell 已经被释放了

    clearListWidgetItems();
}

void QQListWidget::clearListWidgetItems()
{
    while (count() > 0) {
        QListWidgetItem* item = takeItem(0);
        QWidget* widget = itemWidget(item);
        if (widget != nullptr) {
            delete widget; // 释放控件
            widget = nullptr;
        }
        delete item; // 释放Item本身
        item = nullptr;
    }
}

/**
 * @brief QQListWidget::upload
 * 更新
 */
void QQListWidget::upload()
{
    this->clear();
    // 刷新列表
    for (QQCell * cell:cells)
    {
        if (cell->type == QQCellType_Group ||
                cell->type == QQCellType_GroupEx )
        {
            QQListViewGroup *group = new QQListViewGroup();
            group->setGeometry(0, 0, 300, 30);
            if (cell->type == QQCellType_Group)
            {
                // 先获取在线好友
                int nOnlineCnt = 0;
                for (QQCell * child: cell->childs)
                {
                    if (OnLine == child->status) {
                        nOnlineCnt ++;
                    }
                }

                group->ui->groupTitleLabel->setText(cell->groupName);
                group->ui->groupSubTitleLabel->setText(QString("[%1/%2]").arg(nOnlineCnt).arg(cell->childs.size()));
            }
            else
            {
                // 扩展组（如"我的群组"、"最近会话"）只显示组名，不显示人数统计
                group->ui->groupTitleLabel->setText(cell->groupName);
                group->ui->groupSubTitleLabel->setText(QString(""));
            }
            group->setQQCell(cell);
            group->setPopMenu(mGroupMenu);
            connect(group, SIGNAL(onGroupOpenStatusDidChanged(QQListViewGroup*)),
                    this, SLOT(onGroupOpenDidChanged(QQListViewGroup *)));
            connect(group, SIGNAL(onpopmenuwillshow(QQPopMenuWidget*,QMenu*)),
                    this, SLOT(on_popmenuWillShow(QQPopMenuWidget*,QMenu*)));

            QListWidgetItem * item = new QListWidgetItem("");
            this->addItem(item);
            this->setItemWidget(item, group);
            item->setSizeHint(group->geometry().size());

            if (cell->isOpen)
            {
                // 先显示在线好友
                for (QQCell * ccell:cell->childs)
                {
                    if (OnLine == ccell->status)
                        AddChildItem(ccell);
                }

                // 再加载离线好友
                for (QQCell * ccell:cell->childs)
                {
                    if (OffLine == ccell->status)
                        AddChildItem(ccell);
                }
            }
        }
    }
}

void QQListWidget::onGroupOpenDidChanged(QQListViewGroup */*group*/)
{
    upload();
}

void QQListWidget::onChildDidSelected(QQListViewChild *child)
{
    //    qDebug() << "clicked" << child->cell->name;
    Q_UNUSED(child);
}

//void QQListWidget::onChildDidDoubleClicked(QQCell *cell)
//{

//}

void QQListWidget::setBackGroundColor(QWidget *widget,int index)
{
    widget->setStyleSheet(index % 2 ? "QWidget{background:rgb(100, 100, 100, 100)}":
                                      "QWidget{background:rgb(255, 255, 255)}");
}

/**
 * @brief QQListWidget::AddChildItem
 * @param cell
 */
void QQListWidget::AddChildItem(QQCell *cell)
{
    QQListViewChild *child = new QQListViewChild;
    child->setGeometry(0, 0, 300, 60);
    child->cell = cell;
    child->setPopMenu(mChildMenu);
    connect(child, SIGNAL(onChildDidSelected(QQListViewChild*)), this,
            SLOT(onChildDidSelected(QQListViewChild*)));
    connect(child, SIGNAL(onChildDidDoubleClicked(QQCell*)), this,
            SIGNAL(onChildDidDoubleClicked(QQCell*)));
    connect(child, SIGNAL(signalRightClicked(QQCell*)),
            this, SLOT(SltCellRightCicked(QQCell*)));

    QListWidgetItem * item = new QListWidgetItem("");
    this->addItem(item);
    this->setItemWidget(item, child);
    item->setSizeHint(child->geometry().size());
}

QQCell *QQListWidget::getGroupForName(QString *groupName)
{
    for (QQCell *group : cells)
    {
        if (!group->groupName.compare(groupName))
        {
            return group;
        }
    }

    return nullptr;
}

void QQListWidget::on_popmenuWillShow(QQPopMenuWidget *widget, QMenu *menu)
{
    emit on_popmenu_will_show(this, widget, menu);;
}

void QQListWidget::SltCellRightCicked(QQCell *cell)
{
    m_nRightClickCell = cell;
}

QList<QQCell *> QQListWidget::getCells() const
{
    return cells;
}

#include "qqpopmenuwidget.h"

QQPopMenuWidget::QQPopMenuWidget(QWidget *parent) :
    CustomWidget(parent),
    popMenu(nullptr)      // 必须初始化：否则右键时对野指针判空会漏过，exec 直接崩
{
}

void QQPopMenuWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton)
    {
        if (nullptr == popMenu)
        {
            return ;
        }

        emit onpopmenuwillshow(this, popMenu);
        popMenu->exec(QCursor::pos());
    }
}

void QQPopMenuWidget::setPopMenu(QMenu *menu)
{
    popMenu = menu;
}

#include "qqlistviewgroup.h"

QQListViewGroup::QQListViewGroup(QQPopMenuWidget *parent) :
    QQPopMenuWidget(parent),
    ui(new Ui::QQListViewGroup),
    cell(nullptr)         // 未初始化时下面的 cell == nullptr 判空会失效
{
    ui->setupUi(this);

    this->setFixedHeight(25);
    //
    ui->groupSubTitleLabel->setAlignment(Qt::AlignRight);
}

QQListViewGroup::~QQListViewGroup()
{
    delete ui;
}

void QQListViewGroup::setQQCell(QQCell *c)
{
    cell = c;
    if (cell->isOpen)
    {
        ui->groupIconLabel->setPixmap(QPixmap(":/resource/common/aio_arrow_down.png"));
    }
    else
    {
        ui->groupIconLabel->setPixmap(QPixmap(":/resource/common/aio_arrow_right.png"));
    }
}

void QQListViewGroup::mousePressEvent(QMouseEvent *ev)
{
    if (Qt::RightButton == ev->button())
    {
        // 右键交给基类弹菜单。exec() 是阻塞的，用户点"刷新/删除该组"会重建整个列表，
        // 本对象可能已经被 delete —— 所以弹完立刻返回，绝不能再访问 this 的任何成员。
        QQPopMenuWidget::mousePressEvent(ev);
        return;
    }

    if (ev->button() != Qt::LeftButton)
    {
        return ;
    }

    if (cell == nullptr) return;

    cell->isOpen = !cell->isOpen;
    if (cell->isOpen)
    {
        ui->groupIconLabel->setPixmap(QPixmap(":/resource/common/aio_arrow_down.png"));
    }
    else
    {
        ui->groupIconLabel->setPixmap(QPixmap(":/resource/common/aio_arrow_right.png"));
    }

    Q_EMIT onGroupOpenStatusDidChanged(this);
}

#include "groupmemberdialog.h"
#include "comapi/unit.h"
#include "comapi/myapp.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QMessageBox>
#include <QJsonValue>
#include <QColor>
#include <QFont>
#include <QTimer>

GroupMemberDialog::GroupMemberDialog(int groupId, const QString &groupName, QWidget *parent)
    : QDialog(parent),
      m_nGroupId(groupId),
      m_strGroupName(groupName)
{
    setWindowTitle(tr("群成员 - %1").arg(groupName));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    resize(560, 480);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);

    // 顶部说明：人数 / 我的角色
    m_labelTip = new QLabel(tr("正在加载成员…"), this);
    m_labelTip->setStyleSheet("color:#666666;");
    mainLayout->addWidget(m_labelTip);

    // 成员表格
    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels(QStringList()
                                       << tr("昵称") << tr("角色")
                                       << tr("状态") << tr("入群时间"));
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setColumnWidth(0, 180);
    m_table->setColumnWidth(1, 80);
    m_table->setColumnWidth(2, 70);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    mainLayout->addWidget(m_table, 1);

    // 底部按钮
    QHBoxLayout *bottomLayout = new QHBoxLayout;
    m_btnRefresh = new QPushButton(tr("刷新"), this);
    m_btnKick = new QPushButton(tr("移出群聊"), this);
    m_btnTransfer = new QPushButton(tr("转让群主"), this);
    QPushButton *btnClose = new QPushButton(tr("关闭"), this);
    for (QPushButton *btn : {m_btnRefresh, m_btnKick, m_btnTransfer, btnClose}) {
        btn->setFixedWidth(90);
    }

    bottomLayout->addWidget(m_btnRefresh);
    bottomLayout->addStretch();
    bottomLayout->addWidget(m_btnKick);
    bottomLayout->addWidget(m_btnTransfer);
    bottomLayout->addWidget(btnClose);
    mainLayout->addLayout(bottomLayout);

    connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_btnRefresh, &QPushButton::clicked, this, [this]() {
        setBusy(true, tr("正在刷新…"));
        emit signalRequestMembers(m_nGroupId);
    });
    connect(m_btnKick, &QPushButton::clicked, this, &GroupMemberDialog::sltKickClicked);
    connect(m_btnTransfer, &QPushButton::clicked, this, &GroupMemberDialog::sltTransferClicked);
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &GroupMemberDialog::sltTableSelectionChanged);

    // 打开即拉一次。用 singleShot 推到事件循环下一轮，
    // 保证调用方已经把 signalRequestMembers 接上了，否则这次请求会丢。
    QTimer::singleShot(0, this, [this]() { emit signalRequestMembers(m_nGroupId); });
}

/**
 * @brief GroupMemberDialog::setMembers
 * @param obj 服务端下发：{ groupId, ownerId, members: [ {userId,name,identity,online,joinTime} ] }
 */
void GroupMemberDialog::setMembers(const QJsonObject &obj)
{
    if (obj.value("groupId").toInt() != m_nGroupId) return;   // 不是本群的，忽略

    m_nOwnerId = obj.value("ownerId").toInt(-1);
    m_members = obj.value("members").toArray();
    m_bSelfIsOwner = (m_nOwnerId > 0 && m_nOwnerId == MyApp::m_nId);

    rebuildTable();

    // 群主才有管理权限；非群主把按钮灰掉，避免点了报"只有群主才能…"
    m_btnKick->setEnabled(m_bSelfIsOwner);
    m_btnTransfer->setEnabled(m_bSelfIsOwner);

    m_labelTip->setText(tr("共 %1 人 · 你是%2")
                        .arg(m_members.size())
                        .arg(m_bSelfIsOwner ? tr("群主") : tr("普通成员")));
    setBusy(false);
    sltTableSelectionChanged();
}

void GroupMemberDialog::rebuildTable()
{
    m_table->setRowCount(0);

    for (const QJsonValue &value : m_members) {
        const QJsonObject member = value.toObject();
        const int userId = member.value("userId").toInt();
        const bool isOwner = (userId == m_nOwnerId) || (member.value("identity").toInt() == Owner);

        const int row = m_table->rowCount();
        m_table->insertRow(row);

        QString name = member.value("name").toString();
        if (name.isEmpty()) name = tr("用户%1").arg(userId);
        if (userId == MyApp::m_nId) name += tr("（我）");

        QTableWidgetItem *nameItem = new QTableWidgetItem(name);
        nameItem->setData(Qt::UserRole, userId);
        nameItem->setData(Qt::UserRole + 1, isOwner);

        QTableWidgetItem *roleItem = new QTableWidgetItem(isOwner ? tr("群主") : tr("成员"));
        roleItem->setTextAlignment(Qt::AlignCenter);
        if (isOwner) {
            QFont f = roleItem->font();
            f.setBold(true);
            roleItem->setFont(f);
            roleItem->setForeground(QColor("#E64340"));
        }

        const bool online = member.value("online").toBool();
        QTableWidgetItem *stateItem = new QTableWidgetItem(online ? tr("在线") : tr("离线"));
        stateItem->setTextAlignment(Qt::AlignCenter);
        stateItem->setForeground(QColor(online ? "#1a7f37" : "#999999"));

        QString joinTime = member.value("joinTime").toString().trimmed();
        if (joinTime.isEmpty()) joinTime = "-";

        m_table->setItem(row, 0, nameItem);
        m_table->setItem(row, 1, roleItem);
        m_table->setItem(row, 2, stateItem);
        m_table->setItem(row, 3, new QTableWidgetItem(joinTime));
    }
}

int GroupMemberDialog::selectedUserId() const
{
    const QList<QTableWidgetItem *> items = m_table->selectedItems();
    if (items.isEmpty()) return -1;
    QTableWidgetItem *first = m_table->item(items.first()->row(), 0);
    return first ? first->data(Qt::UserRole).toInt() : -1;
}

void GroupMemberDialog::sltTableSelectionChanged()
{
    const int userId = selectedUserId();
    const bool valid = (userId > 0 && userId != MyApp::m_nId);

    // 选中自己不能踢 / 不能转让给自己
    if (!m_bSelfIsOwner) {
        m_btnKick->setEnabled(false);
        m_btnTransfer->setEnabled(false);
        return;
    }

    QTableWidgetItem *first = m_table->selectedItems().isEmpty()
            ? nullptr : m_table->item(m_table->selectedItems().first()->row(), 0);
    const bool targetIsOwner = first && first->data(Qt::UserRole + 1).toBool();

    m_btnKick->setEnabled(valid && !targetIsOwner);
    m_btnTransfer->setEnabled(valid && !targetIsOwner);
}

void GroupMemberDialog::sltKickClicked()
{
    const int userId = selectedUserId();
    if (userId <= 0 || userId == MyApp::m_nId) return;

    QTableWidgetItem *first = m_table->item(m_table->currentRow(), 0);
    const QString name = first ? first->text() : QString::number(userId);

    if (QMessageBox::question(this, tr("移出群聊"),
                              tr("确定把「%1」移出群聊吗？\n移出后他将收不到群消息。").arg(name),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    setBusy(true, tr("正在移出…"));
    emit signalKickMember(m_nGroupId, userId);
}

void GroupMemberDialog::sltTransferClicked()
{
    const int userId = selectedUserId();
    if (userId <= 0 || userId == MyApp::m_nId) return;

    QTableWidgetItem *first = m_table->item(m_table->currentRow(), 0);
    const QString name = first ? first->text() : QString::number(userId);

    if (QMessageBox::question(this, tr("转让群主"),
                              tr("确定把群主转让给「%1」吗？\n转让后你将变成普通成员，"
                                 "不能再管理群成员。").arg(name),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    setBusy(true, tr("正在转让…"));
    emit signalTransferOwner(m_nGroupId, userId);
}

void GroupMemberDialog::setBusy(bool busy, const QString &hint)
{
    if (busy) {
        m_btnRefresh->setEnabled(false);
        m_btnKick->setEnabled(false);
        m_btnTransfer->setEnabled(false);
        if (!hint.isEmpty()) m_labelTip->setText(hint);
    } else {
        m_btnRefresh->setEnabled(true);
        sltTableSelectionChanged();
    }
}

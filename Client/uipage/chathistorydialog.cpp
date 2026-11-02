#include "chathistorydialog.h"
#include "comapi/unit.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidgetItem>
#include <QFont>
#include <QFileInfo>

ChatHistoryDialog::ChatHistoryDialog(const QString &title, QWidget *parent)
    : QDialog(parent), m_title(title)
{
    setWindowTitle(title);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    resize(680, 520);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);

    // 顶部：搜索 + 计数
    QHBoxLayout *topLayout = new QHBoxLayout;
    m_editFilter = new QLineEdit(this);
    m_editFilter->setPlaceholderText(tr("搜索聊天内容…"));
    m_editFilter->setClearButtonEnabled(true);
    m_labelCount = new QLabel(this);
    m_labelCount->setStyleSheet("color:#666666;");
    topLayout->addWidget(m_editFilter, 1);
    topLayout->addWidget(m_labelCount);
    mainLayout->addLayout(topLayout);

    // 中部：记录表格
    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels(QStringList() << tr("时间") << tr("发送者") << tr("内容"));
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setColumnWidth(0, 130);
    m_table->setColumnWidth(1, 110);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    mainLayout->addWidget(m_table, 1);

    // 底部：关闭
    QHBoxLayout *bottomLayout = new QHBoxLayout;
    bottomLayout->addStretch();
    QPushButton *btnClose = new QPushButton(tr("关闭"), this);
    btnClose->setFixedWidth(90);
    bottomLayout->addWidget(btnClose);
    mainLayout->addLayout(bottomLayout);

    connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_editFilter, &QLineEdit::textChanged, this, &ChatHistoryDialog::sltFilterChanged);
}

void ChatHistoryDialog::setMessages(const QVector<QJsonObject> &messages)
{
    m_messages = messages;
    m_filtered = messages;
    rebuild();
}

/**
 * @brief 把消息类型转成给人看的文字
 */
QString ChatHistoryDialog::typeName(int type)
{
    switch (type)
    {
    case Text:    return QStringLiteral("文字");
    case Audio:   return QStringLiteral("语音");
    case Picture: return QStringLiteral("图片");
    case Files:   return QStringLiteral("文件");
    case Face:    return QStringLiteral("表情");
    // 早期版本可能把协议码存进了 type 列
    case SendMsg:
    case SendGroupMsg: return QStringLiteral("文字");
    default:      return QStringLiteral("其他");
    }
}

void ChatHistoryDialog::rebuild()
{
    m_table->setRowCount(m_filtered.size());

    for (int i = 0; i < m_filtered.size(); ++i)
    {
        const QJsonObject &obj = m_filtered.at(i);
        const int type = obj.value("type").toInt();

        // 时间
        m_table->setItem(i, 0, new QTableWidgetItem(obj.value("datetime").toString()));
        // 发送者
        m_table->setItem(i, 1, new QTableWidgetItem(obj.value("name").toString()));

        // 内容：图片/表情/文件不是纯文本，给个可读的占位描述
        QString content = obj.value("content").toString();
        if (Picture == type)
        {
            content = tr("[图片]");
        }
        else if (Face == type)
        {
            content = tr("[表情]");
        }
        else if (Files == type)
        {
            content = tr("[文件] %1").arg(QFileInfo(content).fileName());
        }
        else if (Audio == type)
        {
            const int sec = qMax(1, obj.value("duration").toInt() / 1000);
            content = tr("[语音] %1\"").arg(sec);
        }

        QTableWidgetItem *contentItem = new QTableWidgetItem(content);
        contentItem->setToolTip(content);
        m_table->setItem(i, 2, contentItem);
    }

    m_labelCount->setText(tr("共 %1 条").arg(m_filtered.size()));

    // 默认滚到最底部，用户看到的通常是最近的对话
    if (m_table->rowCount() > 0)
    {
        m_table->scrollToBottom();
    }
}

void ChatHistoryDialog::sltFilterChanged(const QString &keyword)
{
    const QString key = keyword.trimmed();
    if (key.isEmpty())
    {
        m_filtered = m_messages;
        rebuild();
        return;
    }

    m_filtered.clear();
    for (const QJsonObject &obj : m_messages)
    {
        const bool hit = obj.value("content").toString().contains(key, Qt::CaseInsensitive)
                || obj.value("name").toString().contains(key, Qt::CaseInsensitive)
                || obj.value("datetime").toString().contains(key, Qt::CaseInsensitive);
        if (hit) m_filtered.append(obj);
    }
    rebuild();
}

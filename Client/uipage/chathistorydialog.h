#ifndef CHATHISTORYDIALOG_H
#define CHATHISTORYDIALOG_H

#include <QDialog>
#include <QVector>
#include <QJsonObject>

class QLineEdit;
class QTableWidget;
class QLabel;

/**
 * @brief 聊天记录窗口
 *
 * 展示与某个好友（或某个群）的全部历史消息，支持关键字过滤。
 * 数据由调用方通过 setMessages() 传入，本类只负责展示。
 */
class ChatHistoryDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChatHistoryDialog(const QString &title, QWidget *parent = nullptr);

    void setMessages(const QVector<QJsonObject> &messages);

private slots:
    void sltFilterChanged(const QString &keyword);

private:
    void rebuild();
    static QString typeName(int type);

private:
    QString             m_title;
    QVector<QJsonObject> m_messages;
    QVector<QJsonObject> m_filtered;

    QLineEdit    *m_editFilter = nullptr;
    QTableWidget *m_table      = nullptr;
    QLabel       *m_labelCount = nullptr;
};

#endif // CHATHISTORYDIALOG_H

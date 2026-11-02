#ifndef GROUPMEMBERDIALOG_H
#define GROUPMEMBERDIALOG_H

#include <QDialog>
#include <QJsonObject>
#include <QJsonArray>

class QTableWidget;
class QLabel;
class QPushButton;

/**
 * @brief 群成员管理窗口
 *
 * 展示某个群的成员（昵称 / 角色 / 在线状态 / 入群时间），群主可以在这里
 * 把人移出群聊、或者把群主转让给别人。网络请求本身不在这里做：
 * 本类只发信号，由 MainWindow 负责收发协议，这样界面和数据层不纠缠。
 */
class GroupMemberDialog : public QDialog
{
    Q_OBJECT

public:
    explicit GroupMemberDialog(int groupId, const QString &groupName,
                               QWidget *parent = nullptr);

    int groupId() const { return m_nGroupId; }

    /// 服务器下发成员列表后由 MainWindow 调用
    void setMembers(const QJsonObject &obj);

signals:
    void signalRequestMembers(int groupId);                  // 请求（刷新）成员列表
    void signalKickMember(int groupId, int userId);          // 移出群聊
    void signalTransferOwner(int groupId, int userId);       // 转让群主

private slots:
    void sltKickClicked();
    void sltTransferClicked();
    void sltTableSelectionChanged();

private:
    void rebuildTable();
    int  selectedUserId() const;
    void setBusy(bool busy, const QString &hint = QString());

private:
    int        m_nGroupId = 0;
    QString    m_strGroupName;
    int        m_nOwnerId = -1;          // 当前群主 id
    bool       m_bSelfIsOwner = false;   // 我自己是不是群主
    QJsonArray m_members;

    QLabel       *m_labelTip   = nullptr;
    QTableWidget *m_table      = nullptr;
    QPushButton  *m_btnRefresh = nullptr;
    QPushButton  *m_btnKick    = nullptr;
    QPushButton  *m_btnTransfer = nullptr;
};

#endif // GROUPMEMBERDIALOG_H

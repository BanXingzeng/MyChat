#ifndef QQCELL_H
#define QQCELL_H

#include <QString>
#include <QList>

typedef enum tagQQCellType_e
{
    QQCellType_Group = 0,
    QQCellType_Child = 1,
    QQCellType_GroupEx = 2,
} QQCellType_e;

/**
            Model数据模型
 * @brief 监听鼠标右键事件，针对 QQCell（组/子项）数据项触发对应弹出菜单展示
 */
class QQCell
{
public:
    QQCell();

    void SetSubtitle(const QString &text);
    void SetIconPath(const QString &path);
    void SetStatus(const quint8 &val);
public:
    QString iconPath;
    QString name;
    QString subTitle;

    QString ipaddr;
    QString groupName;      //组名
    int     id;             //QQ号，作为标识
    int     status;

    int     unread;         // 未读消息条数（0 表示不显示角标）
    bool    isGroup;        // 会话列表里标记该会话是群组还是好友

    //
    QQCellType_e type;

    //下面几项都是用于组(QQCellType_Group)状态, 对于子(QQCellType_Child)状态无用
    //是否是打开状态
    bool isOpen;
    //子项
    QList<QQCell *> childs;
};

#endif // QQCELL_H

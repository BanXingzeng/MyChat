#ifndef SCREENSHOTOVERLAY_H
#define SCREENSHOTOVERLAY_H

#include <QWidget>
#include <QPixmap>
#include <QRect>
#include <QPoint>

/**
 * @brief 截屏选区覆盖层
 *
 * 用法：先抓一张整个桌面的 QPixmap，交给本类全屏显示，
 * 用户拖拽框选后通过 captured() 抛出裁剪好的图片；ESC / 右键取消。
 */
class ScreenshotOverlay : public QWidget
{
    Q_OBJECT

public:
    /// @param desktop 已经抓好的桌面截图
    /// @param desktopRect 截图对应的桌面逻辑坐标（多屏时为虚拟桌面范围）
    explicit ScreenshotOverlay(const QPixmap &desktop,
                               const QRect &desktopRect,
                               QWidget *parent = nullptr);

    /// 抓取全部屏幕拼成一张图，返回虚拟桌面范围
    static QPixmap grabDesktop(QRect *desktopRect);

Q_SIGNALS:
    void captured(const QPixmap &pixmap);
    void cancelled();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QRect currentSelection() const;

private:
    QPixmap m_desktop;
    QRect   m_desktopRect;
    QPoint  m_origin;
    QPoint  m_current;
    bool    m_pressing = false;
};

#endif // SCREENSHOTOVERLAY_H

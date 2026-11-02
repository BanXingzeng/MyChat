#include "screenshotoverlay.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QDebug>

ScreenshotOverlay::ScreenshotOverlay(const QPixmap &desktop,
                                     const QRect &desktopRect,
                                     QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
      m_desktop(desktop),
      m_desktopRect(desktopRect)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    // 用虚拟桌面坐标定位，多屏时也能覆盖全部
    setGeometry(m_desktopRect);
}

/**
 * @brief ScreenshotOverlay::grabDesktop
 * 抓取全部屏幕并拼接成一张图（每块屏幕单独抓，避免只抓到主屏）
 */
QPixmap ScreenshotOverlay::grabDesktop(QRect *desktopRect)
{
    QRect virtualGeo;
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens)
    {
        virtualGeo = virtualGeo.united(screen->geometry());
    }
    if (virtualGeo.isEmpty())
    {
        if (desktopRect) *desktopRect = QRect();
        return QPixmap();
    }

    QPixmap desktop(virtualGeo.size());
    desktop.fill(Qt::black);

    QPainter painter(&desktop);
    for (QScreen *screen : screens)
    {
        QPixmap shot = screen->grabWindow(0);
        if (shot.isNull()) continue;

        // 有的环境下 grabWindow 会带上设备像素比，这里统一成逻辑像素
        shot.setDevicePixelRatio(1.0);
        const QRect geo = screen->geometry();
        if (shot.size() != geo.size())
        {
            shot = shot.scaled(geo.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        painter.drawPixmap(geo.topLeft() - virtualGeo.topLeft(), shot);
    }
    painter.end();

    if (desktopRect) *desktopRect = virtualGeo;
    return desktop;
}

/**
 * @brief 当前选区（鼠标按下点与当前位置围成的矩形，屏幕坐标转成控件坐标）
 */
QRect ScreenshotOverlay::currentSelection() const
{
    const QPoint topLeft = m_desktopRect.topLeft();
    return QRect(m_origin - topLeft, m_current - topLeft).normalized();
}

void ScreenshotOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);

    // 底图（截到的桌面原样）
    painter.drawPixmap(rect(), m_desktop);

    // 整体压暗，选区再单独提亮
    painter.fillRect(rect(), QColor(0, 0, 0, 120));

    const QRect sel = m_pressing ? currentSelection() : QRect();
    if (!sel.isEmpty())
    {
        painter.drawPixmap(sel, m_desktop, sel);

        QPen pen(QColor("#05CE26"));
        pen.setWidth(1);
        painter.setPen(pen);
        painter.drawRect(sel.adjusted(0, 0, -1, -1));

        // 四个角做小标记，方便对齐
        painter.fillRect(sel.left() - 2, sel.top() - 2, 5, 5, QColor("#05CE26"));
        painter.fillRect(sel.right() - 2, sel.top() - 2, 5, 5, QColor("#05CE26"));
        painter.fillRect(sel.left() - 2, sel.bottom() - 2, 5, 5, QColor("#05CE26"));
        painter.fillRect(sel.right() - 2, sel.bottom() - 2, 5, 5, QColor("#05CE26"));

        // 尺寸提示
        const QString tip = QString("%1 × %2").arg(sel.width()).arg(sel.height());
        QFont font = painter.font();
        font.setPointSize(9);
        painter.setFont(font);
        const QFontMetrics fm(font);
        const int tw = fm.width(tip) + 12;
        QRect tipRect(sel.left(), sel.top() - fm.height() - 8, tw, fm.height() + 4);
        if (tipRect.top() < 0) tipRect.moveTop(sel.top() + 4);

        painter.fillRect(tipRect, QColor(0, 0, 0, 170));
        painter.setPen(QColor("#FFFFFF"));
        painter.drawText(tipRect, Qt::AlignCenter, tip);
    }
    else if (!m_pressing)
    {
        // 未开始时给一句操作提示
        painter.setPen(QColor("#FFFFFF"));
        QFont font = painter.font();
        font.setPointSize(11);
        painter.setFont(font);
        painter.drawText(rect(), Qt::AlignCenter,
                         tr("拖动鼠标框选要发送的区域，Esc 或右键取消"));
    }
}

void ScreenshotOverlay::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton)
    {
        emit cancelled();
        close();
        return;
    }
    if (event->button() != Qt::LeftButton) return;

    m_pressing = true;
    m_origin = event->globalPos();
    m_current = m_origin;
    update();
}

void ScreenshotOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_pressing) return;
    m_current = event->globalPos();
    update();
}

void ScreenshotOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_pressing) return;

    m_pressing = false;
    m_current = event->globalPos();

    QRect sel = currentSelection();
    // 只是点了一下，当成误触
    if (sel.width() < 5 || sel.height() < 5)
    {
        update();
        return;
    }

    // 选区不能超出底图
    sel = sel.intersected(m_desktop.rect());
    if (sel.isEmpty())
    {
        update();
        return;
    }

    const QPixmap shot = m_desktop.copy(sel);
    emit captured(shot);
    close();
}

void ScreenshotOverlay::mouseDoubleClickEvent(QMouseEvent *)
{
    // 双击等同取消，避免误操作直接发出去
    emit cancelled();
    close();
}

void ScreenshotOverlay::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape)
    {
        emit cancelled();
        close();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        const QRect sel = currentSelection().intersected(m_desktop.rect());
        if (!sel.isEmpty() && sel.width() >= 5 && sel.height() >= 5)
        {
            emit captured(m_desktop.copy(sel));
            close();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

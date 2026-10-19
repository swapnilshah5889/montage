#include "VideoSurface.h"

#include "Player.h"

#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

namespace {

class FrameNode : public QSGSimpleTextureNode
{
public:
    ~FrameNode() override { delete texture(); }

    void replaceTexture(QSGTexture *t)
    {
        QSGTexture *old = texture();
        setTexture(t);
        delete old;
    }
};

}  // namespace

VideoSurface::VideoSurface(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    if (Player *player = Player::instance()) {
        m_frame = player->lastFrame();
        m_newFrame = !m_frame.isNull();
        connect(player, &Player::frameReady, this, [this](const QImage &img) {
            const bool sizeChanged = img.size() != m_frame.size();
            m_frame = img;
            m_newFrame = true;
            update();
            if (sizeChanged) emit frameRectChanged();
        });
    }
}

QRectF VideoSurface::frameRect() const
{
    if (m_frame.isNull() || width() <= 0 || height() <= 0) return {};
    const QSizeF s = QSizeF(m_frame.size()).scaled(size(), Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - s.width()) / 2, (height() - s.height()) / 2), s);
}

void VideoSurface::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        emit frameRectChanged();
        update();
    }
}

QSGNode *VideoSurface::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *node = static_cast<FrameNode *>(old);
    if (m_frame.isNull()) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = new FrameNode;
        node->setFiltering(QSGTexture::Linear);
        m_newFrame = true;
    }
    if (m_newFrame) {
        node->replaceTexture(window()->createTextureFromImage(m_frame));
        m_newFrame = false;
    }
    node->setRect(frameRect());
    return node;
}

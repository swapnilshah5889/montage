#pragma once

#include <QImage>
#include <QQuickItem>

// Shows the player's latest frame as a scene-graph texture, letterboxed to
// the item. Same approach as CapCut's player (QSGSimpleTextureNode).
class VideoSurface : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QRectF frameRect READ frameRect NOTIFY frameRectChanged)

public:
    explicit VideoSurface(QQuickItem *parent = nullptr);
    QRectF frameRect() const;

signals:
    void frameRectChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    QImage m_frame;
    bool m_newFrame = false;
};

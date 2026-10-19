import QtQuick
import QtQuick.Controls
import Montage

// One clip on the timeline: drag to move (across tracks too), drag either
// edge to trim. Edits are committed once, on release.
Rectangle {
    id: item
    required property var seg
    required property string trackId
    required property var timeline

    property real trimL: 0  // pixels, while dragging the left edge
    property real trimR: 0  // pixels, while dragging the right edge
    property bool moving: false
    readonly property bool selected: Project.selectedId === seg.id

    x: timeline.xOf(seg.start) + trimL
    y: 3
    width: Math.max(6, timeline.xOf(seg.duration) - trimL + trimR)
    height: parent ? parent.height - 6 : 40
    z: moving ? 10 : 0
    radius: 5
    clip: true
    color: Theme.kindColor(seg.kind)
    opacity: moving ? 0.85 : 1
    border.width: selected ? 2 : 0
    border.color: "white"

    function restore() {
        moving = false
        x = Qt.binding(() => timeline.xOf(seg.start) + trimL)
        y = 3
        if (parent) parent.z = 0
    }

    Image {
        anchors.fill: parent
        anchors.margins: 2
        visible: item.seg.kind !== "audio" && item.seg.thumbnail !== ""
        source: item.seg.thumbnail
        fillMode: Image.TileHorizontally
        horizontalAlignment: Image.AlignLeft
        sourceSize.height: Math.max(1, height)
        asynchronous: true
        opacity: 0.9
    }

    // Audio clips get a simple level strip instead of thumbnails.
    Rectangle {
        visible: item.seg.kind === "audio"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.margins: 6
        height: Math.max(2, (parent.height - 22) * Math.min(1, item.seg.volume))
        anchors.verticalCenterOffset: 6
        radius: 2
        color: Qt.lighter(Theme.audio, 1.5)
        opacity: 0.35
    }

    Label {
        x: 8
        y: 4
        visible: item.width > 34
        width: Math.min(implicitWidth, item.width - 16)
        text: (item.seg.kind === "audio" ? "♪ " : item.seg.kind === "text" ? "T  " : "") + item.seg.name + "   " + Project.formatTime(item.seg.duration, false)
        elide: Text.ElideRight
        color: "white"
        font.pixelSize: 11
        leftPadding: 4
        rightPadding: 4
        background: Rectangle { color: "#99000000"; radius: 3 }
    }

    MouseArea {
        id: body
        anchors.fill: parent
        anchors.leftMargin: 7
        anchors.rightMargin: 7
        preventStealing: true
        drag.target: item
        drag.axis: Drag.XAndYAxis
        drag.threshold: 3
        drag.minimumX: 0
        cursorShape: item.moving ? Qt.ClosedHandCursor : Qt.PointingHandCursor
        property real x0
        property real y0

        onPressed: {
            Project.selectedId = item.seg.id
            x0 = item.x
            y0 = item.y
            item.parent.z = 10  // keep the clip above neighbouring lanes
        }
        onPositionChanged: {
            if (!pressed) return
            if (!item.moving && (Math.abs(item.x - x0) > 2 || Math.abs(item.y - y0) > 2)) item.moving = true
            if (item.moving) item.x = item.timeline.xOf(item.timeline.snapMove(item.seg, item.timeline.usAt(item.x)))
        }
        onReleased: {
            const moved = item.moving
            if (moved) item.timeline.dropSegment(item)
            item.restore()
        }
        onCanceled: item.restore()
    }

    // Edge grips
    Repeater {
        model: 2
        Rectangle {
            required property int index
            visible: item.selected
            x: index === 0 ? 2 : item.width - 6
            y: (item.height - height) / 2
            width: 4
            height: Math.min(22, item.height - 12)
            radius: 2
            color: "white"
            opacity: 0.85
        }
    }

    MouseArea {
        width: 8
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        cursorShape: Qt.SizeHorCursor
        preventStealing: true
        property real startX

        onPressed: (m) => {
            Project.selectedId = item.seg.id
            startX = mapToItem(item.timeline, m.x, 0).x
        }
        onPositionChanged: (m) => {
            if (!pressed) return
            const t = item.timeline
            const s = item.seg
            const dx = mapToItem(t, m.x, 0).x - startX
            const minStart = (s.kind === "image" || s.kind === "text") ? -1e15 : s.start - s.sourceStart
            let start = t.snapEdge(s.start + dx * t.usPerPx, s.id)
            start = Math.max(minStart, Math.min(start, s.start + s.duration - 100000))
            item.trimL = t.xOf(start - s.start)
        }
        onReleased: {
            const d = item.trimL * item.timeline.usPerPx
            const id = item.seg.id, start = item.seg.start, dur = item.seg.duration
            item.trimL = 0
            if (Math.abs(d) >= 1) Qt.callLater(() => Project.trimSegment(id, start + d, dur - d))
        }
    }

    MouseArea {
        width: 8
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        cursorShape: Qt.SizeHorCursor
        preventStealing: true
        property real startX

        onPressed: (m) => {
            Project.selectedId = item.seg.id
            startX = mapToItem(item.timeline, m.x, 0).x
        }
        onPositionChanged: (m) => {
            if (!pressed) return
            const t = item.timeline
            const s = item.seg
            const dx = mapToItem(t, m.x, 0).x - startX
            const maxEnd = (s.kind === "image" || s.kind === "text") ? 1e15 : s.start + (s.materialDuration - s.sourceStart)
            let end = t.snapEdge(s.start + s.duration + dx * t.usPerPx, s.id)
            end = Math.min(maxEnd, Math.max(end, s.start + 100000))
            item.trimR = t.xOf(end - (s.start + s.duration))
        }
        onReleased: {
            const d = item.trimR * item.timeline.usPerPx
            const id = item.seg.id, start = item.seg.start, dur = item.seg.duration
            item.trimR = 0
            if (Math.abs(d) >= 1) Qt.callLater(() => Project.trimSegment(id, start, dur + d))
        }
    }
}

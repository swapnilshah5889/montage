import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montage

Rectangle {
    id: tl
    color: Theme.panel

    property real pps: 60  // pixels per second
    readonly property real usPerPx: 1e6 / pps
    readonly property real snapPx: 8
    readonly property string materialMime: "application/x-montage-material"

    function xOf(us) { return us / 1e6 * pps }
    function usAt(x) { return Math.max(0, x / pps * 1e6) }
    function laneHeight(role) { return role === "main" ? 66 : role === "audio" ? 44 : 50 }

    function snapEdge(us, excludeId) {
        return Project.snap(us, snapPx * usPerPx, Player.position, excludeId)
    }
    function snapMove(seg, start) {
        const a = Project.snap(start, snapPx * usPerPx, Player.position, seg.id)
        if (a !== start) return a
        const end = start + seg.duration
        const b = Project.snap(end, snapPx * usPerPx, Player.position, seg.id)
        return b !== end ? b - seg.duration : start
    }

    // Which lane is under a y position in the track column.
    function laneAt(y) {
        if (y < 0) return "new-overlay"
        const lane = tracksCol.childAt(10, y)
        return lane && lane.laneId !== undefined ? lane.laneId : ""
    }

    function dropSegment(item) {
        const center = item.mapToItem(tracksCol, 0, item.height / 2)
        const lane = laneAt(center.y)
        const id = item.seg.id
        const start = usAt(item.x)
        const fallback = item.trackId
        // Defer: the edit rebuilds the lanes, including this delegate.
        Qt.callLater(() => Project.moveSegment(id, lane === "" ? fallback : lane, start))
    }

    function dropMaterial(drop, laneId, snap) {
        const id = drop.getDataAsString(materialMime)
        const at = snap ? snapEdge(usAt(drop.x), "") : usAt(drop.x)
        drop.accept(Qt.CopyAction)
        Qt.callLater(() => Project.addToTimeline(id, laneId, at))
    }

    function setZoom(p) {
        p = Math.max(4, Math.min(600, p))
        const anchorUs = Player.position
        const screenX = xOf(anchorUs) - flick.contentX
        pps = p
        flick.contentX = Math.max(0, Math.min(xOf(anchorUs) - screenX, flick.contentWidth - flick.width))
        zoom.value = Math.log(p)
    }
    function fit() {
        setZoom((flick.width - 80) / Math.max(1, Project.duration / 1e6))
        flick.contentX = 0
    }

    Connections {
        target: Player
        function onPositionChanged() {
            if (!Player.playing) return
            const px = tl.xOf(Player.position)
            if (px > flick.contentX + flick.width - 30 || px < flick.contentX)
                flick.contentX = Math.max(0, Math.min(px - 30, flick.contentWidth - flick.width))
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 40
            color: Theme.panel
            Rectangle { width: parent.width; height: 1; color: Theme.border }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 12
                spacing: 4

                FlatButton { text: "Split"; tip: "Split at playhead (⌘B)"; enabled: Project.duration > 0; onClicked: Project.splitAt(Player.position) }
                FlatButton { text: "Delete"; tip: "Delete selected clip (⌫)"; enabled: Project.selectedId !== ""; onClicked: Project.deleteSelected() }
                FlatButton { text: "+ Text"; tip: "Add a title at the playhead"; onClicked: Project.addText("Your text", Player.position, 0, "title") }
                Item { Layout.fillWidth: true }
                Label { text: "Zoom"; color: Theme.textDim; font.pixelSize: 12 }
                Slider {
                    id: zoom
                    focusPolicy: Qt.NoFocus
                    implicitWidth: 160
                    from: Math.log(4)
                    to: Math.log(600)
                    value: Math.log(60)
                    onMoved: tl.setZoom(Math.exp(value))
                }
                FlatButton { text: "Fit"; tip: "Fit the whole project"; enabled: Project.duration > 0; onClicked: tl.fit() }
            }
        }

        TimeRuler {
            Layout.fillWidth: true
            implicitHeight: 26
            pps: tl.pps
            contentX: flick.contentX
            onSeek: (us) => Player.seek(us)
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            contentWidth: Math.max(width, tl.xOf(Project.duration) + width * 0.6)
            contentHeight: Math.max(height, tracksCol.y + tracksCol.height + 24)
            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            WheelHandler {
                acceptedModifiers: Qt.ControlModifier
                onWheel: (ev) => tl.setZoom(tl.pps * Math.pow(1.002, ev.angleDelta.y))
            }

            Item {
                id: content
                width: flick.contentWidth
                height: flick.contentHeight

                MouseArea {
                    anchors.fill: parent
                    onPressed: (m) => {
                        content.forceActiveFocus()
                        Project.selectedId = ""
                        Player.seek(tl.usAt(m.x))
                    }
                }

                Column {
                    id: tracksCol
                    y: 6
                    width: parent.width
                    spacing: 4

                    Rectangle {
                        readonly property string laneId: "new-overlay"
                        width: tracksCol.width
                        height: 20
                        radius: 4
                        color: overlayDrop.containsDrag ? "#331fd1c1" : "transparent"
                        Label {
                            x: flick.contentX + 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Drop here for a new overlay track"
                            color: Theme.textDim
                            font.pixelSize: 10
                            opacity: overlayDrop.containsDrag ? 1 : 0.45
                        }
                        DropArea {
                            id: overlayDrop
                            anchors.fill: parent
                            keys: [tl.materialMime]
                            onDropped: (drop) => tl.dropMaterial(drop, "new-overlay", true)
                        }
                    }

                    Repeater {
                        model: Project.tracks
                        delegate: Rectangle {
                            id: lane
                            required property var modelData
                            readonly property string laneId: modelData.id
                            width: tracksCol.width
                            height: tl.laneHeight(modelData.role)
                            radius: 4
                            color: drop.containsDrag ? "#26393a" : modelData.role === "main" ? Theme.mainLane : Theme.lane

                            Label {
                                x: flick.contentX + 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: lane.modelData.role === "main" ? "Main track" : lane.modelData.role === "audio" ? "Audio" : "Overlay"
                                color: Theme.textDim
                                opacity: 0.45
                                font.pixelSize: 11
                            }

                            DropArea {
                                id: drop
                                anchors.fill: parent
                                keys: [tl.materialMime]
                                onDropped: (d) => tl.dropMaterial(d, lane.laneId, lane.modelData.role !== "main")
                            }

                            Repeater {
                                model: lane.modelData.segments
                                delegate: SegmentItem {
                                    required property var modelData
                                    seg: modelData
                                    trackId: lane.laneId
                                    timeline: tl
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    x: tl.xOf(Player.position) - 1
                    width: 2
                    height: content.height
                    color: Theme.playhead
                    z: 50
                }
            }
        }
    }
}

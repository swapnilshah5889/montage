import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montage

Rectangle {
    id: panel
    color: Theme.bg

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            id: stage
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            // Render at the on-screen pixel size, settled after resizes.
            Timer {
                id: boxTimer
                interval: 120
                onTriggered: Player.setPreviewBox(Math.round(surface.width * Screen.devicePixelRatio),
                                                  Math.round(surface.height * Screen.devicePixelRatio))
            }
            onWidthChanged: boxTimer.restart()
            onHeightChanged: boxTimer.restart()
            Component.onCompleted: boxTimer.restart()

            VideoSurface {
                id: surface
                anchors.fill: parent
                anchors.margins: 14
            }

            Label {
                anchors.centerIn: parent
                visible: Project.duration === 0
                text: "Add clips to the timeline to preview them here"
                color: Theme.textDim
            }

            // On-canvas move/scale for the selected picture.
            Item {
                id: box
                readonly property var s: Project.selection
                readonly property rect fr: surface.frameRect
                readonly property bool active: s.id !== undefined && s.kind !== "audio" && s.width > 0
                                               && Player.position >= s.start && Player.position < s.start + s.duration
                readonly property real fit: active ? Math.min(fr.width / s.width, fr.height / s.height) : 1
                visible: active && fr.width > 0
                width: active ? s.width * fit * s.scale : 0
                height: active ? s.height * fit * s.scale : 0
                x: active ? surface.x + fr.x + fr.width / 2 * (1 + s.x) - width / 2 : 0
                y: active ? surface.y + fr.y + fr.height / 2 * (1 + s.y) - height / 2 : 0

                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.color: Theme.accent
                    border.width: 1.5
                }
                Repeater {
                    model: 4
                    Rectangle {
                        required property int index
                        width: 8; height: 8; radius: 4
                        color: Theme.accent
                        x: (index % 2) * box.width - 4
                        y: Math.floor(index / 2) * box.height - 4
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.SizeAllCursor
                    property point p0
                    property real x0
                    property real y0
                    onPressed: (m) => {
                        p0 = mapToItem(stage, m.x, m.y)
                        x0 = box.s.x
                        y0 = box.s.y
                    }
                    onPositionChanged: (m) => {
                        if (!pressed) return
                        const p = mapToItem(stage, m.x, m.y)
                        Project.setSegmentProperty(box.s.id, "x", x0 + (p.x - p0.x) / (box.fr.width / 2), false)
                        Project.setSegmentProperty(box.s.id, "y", y0 + (p.y - p0.y) / (box.fr.height / 2), false)
                    }
                    onReleased: Project.setSegmentProperty(box.s.id, "x", box.s.x, true)
                    onWheel: (w) => {
                        Project.setSegmentProperty(box.s.id, "scale", box.s.scale * Math.pow(1.0015, w.angleDelta.y), false)
                        wheelCommit.restart()
                    }
                    Timer {
                        id: wheelCommit
                        interval: 400
                        onTriggered: Project.setSegmentProperty(box.s.id, "scale", box.s.scale, true)
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 46
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 4

                Label {
                    text: Project.formatTime(Player.position)
                    color: Theme.accent
                    font.family: "Menlo"
                    font.pixelSize: 12
                }
                Label {
                    text: "/ " + Project.formatTime(Project.duration)
                    color: Theme.textDim
                    font.family: "Menlo"
                    font.pixelSize: 12
                }

                Item { Layout.fillWidth: true }

                FlatButton { text: "⏮"; tip: "Go to start"; onClicked: Player.seek(0) }
                FlatButton { text: "◂"; tip: "Previous frame (←)"; onClicked: Player.step(-1) }
                FlatButton {
                    text: Player.playing ? "❚❚" : "▶"
                    tip: "Play / pause (Space)"
                    font.pixelSize: 16
                    implicitWidth: 44
                    enabled: Project.duration > 0
                    onClicked: Player.toggle()
                }
                FlatButton { text: "▸"; tip: "Next frame (→)"; onClicked: Player.step(1) }
                FlatButton { text: "⏭"; tip: "Go to end"; onClicked: Player.seek(Project.duration) }

                Item { Layout.fillWidth: true }

                Label { text: "Ratio"; color: Theme.textDim; font.pixelSize: 12 }
                ComboBox {
                    id: ratio
                    focusPolicy: Qt.NoFocus
                    implicitWidth: 110
                    model: ["Original", "16:9", "9:16", "1:1", "4:3", "3:4", "21:9"]
                    currentIndex: Math.max(0, model.indexOf(Project.aspect))
                    onActivated: Project.setAspect(currentText)
                }
                Label {
                    text: Project.canvasWidth + "×" + Project.canvasHeight + " · " + Math.round(Project.fps * 100) / 100 + " fps"
                    color: Theme.textDim
                    font.pixelSize: 11
                    Layout.leftMargin: 6
                }
            }
        }
    }
}

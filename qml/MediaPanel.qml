import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montage

Rectangle {
    id: panel
    signal importRequested

    color: Theme.panel

    function addToTimeline(material) {
        // Pictures append to the main track; audio lands at the playhead.
        const at = material.kind === "audio" ? Player.position : -1
        Project.addToTimeline(material.id, "", at)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Label { text: "Media"; font.pixelSize: 14; font.bold: true; color: Theme.text }
            Label { text: grid.count > 0 ? grid.count : ""; color: Theme.textDim; font.pixelSize: 12 }
            Item { Layout.fillWidth: true }
            FlatButton { text: "+ Import"; accent: true; tip: "Import media (⌘I)"; onClicked: panel.importRequested() }
        }
        Label {
            Layout.fillWidth: true
            visible: Agent.mediaStatus !== ""
            text: "◔ " + Agent.mediaStatus
            color: Theme.textDim
            font.pixelSize: 11
            elide: Text.ElideRight
        }

        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            readonly property int columns: Math.max(1, Math.floor(width / 140))
            cellWidth: Math.floor(width / columns)
            cellHeight: Math.round((cellWidth - 8) * 0.5625) + 30
            model: Project.materials
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: cell
                required property var modelData
                width: grid.cellWidth
                height: grid.cellHeight

                Drag.active: dragHandler.active
                Drag.dragType: Drag.Automatic
                Drag.supportedActions: Qt.CopyAction
                Drag.imageSource: cell.modelData.thumbnail
                Drag.mimeData: { "application/x-montage-material": cell.modelData.id }

                HoverHandler { id: hover }
                DragHandler { id: dragHandler; target: null }
                TapHandler {
                    onDoubleTapped: panel.addToTimeline(cell.modelData)
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: menu.popup()
                }

                Menu {
                    id: menu
                    MenuItem { text: "Add to timeline"; onTriggered: panel.addToTimeline(cell.modelData) }
                    MenuItem { text: "Remove from project"; onTriggered: Project.removeMaterial(cell.modelData.id) }
                }

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 4
                    radius: Theme.radius
                    color: hover.hovered ? Theme.panelAlt : "transparent"

                    Rectangle {
                        id: thumb
                        width: parent.width
                        height: Math.round(width * 0.5625)
                        radius: Theme.radius
                        color: "#0b0b0c"
                        clip: true

                        Image {
                            anchors.fill: parent
                            source: cell.modelData.thumbnail
                            visible: cell.modelData.thumbnail !== ""
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }
                        Label {
                            anchors.centerIn: parent
                            visible: cell.modelData.kind === "audio"
                            text: "♪"
                            font.pixelSize: 30
                            color: Theme.audio
                        }
                        Rectangle {
                            visible: cell.modelData.used
                            x: 6; y: 6
                            width: 7; height: 7; radius: 4
                            color: Theme.accent
                        }
                        Label {  // shots found by the scene index
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.margins: 5
                            visible: !!cell.modelData.shots && cell.modelData.shots.count > 1
                            text: cell.modelData.shots ? cell.modelData.shots.count + " shots" : ""
                            font.pixelSize: 10
                            color: "white"
                            leftPadding: 4; rightPadding: 4; topPadding: 1; bottomPadding: 1
                            background: Rectangle { color: "#b0000000"; radius: 3 }
                        }
                        Label {
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 5
                            text: cell.modelData.kind === "image" ? "Photo" : Project.formatTime(cell.modelData.duration, false)
                            font.pixelSize: 10
                            color: "white"
                            leftPadding: 4; rightPadding: 4; topPadding: 1; bottomPadding: 1
                            background: Rectangle { color: "#b0000000"; radius: 3 }
                        }
                        FlatButton {
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: 4
                            visible: hover.hovered
                            text: "+"
                            accent: true
                            padding: 0
                            leftPadding: 0
                            rightPadding: 0
                            width: 24
                            height: 24
                            tip: "Add to timeline"
                            onClicked: panel.addToTimeline(cell.modelData)
                        }
                    }
                    Label {
                        anchors.top: thumb.bottom
                        anchors.topMargin: 4
                        width: parent.width
                        text: cell.modelData.name
                        elide: Text.ElideMiddle
                        color: Theme.textDim
                        font.pixelSize: 11
                        ToolTip.visible: nameHover.hovered && !!cell.modelData.shots && cell.modelData.shots.summary !== ""
                        ToolTip.text: cell.modelData.shots ? (cell.modelData.shots.count > 1 ? cell.modelData.shots.count + " shots · best: " : "") + cell.modelData.shots.summary : ""
                        HoverHandler { id: nameHover }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: grid.count === 0
                horizontalAlignment: Text.AlignHCenter
                text: "Import or drop video, audio\nand photos here"
                color: Theme.textDim
                lineHeight: 1.3
            }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montage

Rectangle {
    id: bar
    signal newRequested
    signal openRequested
    signal saveRequested
    signal exportRequested

    implicitHeight: 50
    color: Theme.panel

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 6

        Label {
            text: "Montage"
            font.pixelSize: 16
            font.bold: true
            color: Theme.accent
        }
        Rectangle { implicitWidth: 1; implicitHeight: 22; color: Theme.border; Layout.leftMargin: 6; Layout.rightMargin: 6 }
        FlatButton { text: "New"; onClicked: bar.newRequested() }
        FlatButton { text: "Open…"; tip: "Open project (⌘O)"; onClicked: bar.openRequested() }
        FlatButton { text: "Save"; tip: "Save project (⌘S)"; onClicked: bar.saveRequested() }

        Item { Layout.fillWidth: true }

        TextField {
            id: nameField
            Layout.preferredWidth: 280
            text: Project.name
            color: Theme.text
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: 14
            selectByMouse: true
            onEditingFinished: {
                Project.name = text
                focus = false
            }
            background: Rectangle {
                radius: 6
                color: nameField.activeFocus ? Theme.panelAlt : "transparent"
                border.color: nameField.activeFocus ? Theme.accent : "transparent"
            }
        }

        Item { Layout.fillWidth: true }

        FlatButton { text: "Undo"; tip: "⌘Z"; enabled: Project.canUndo; onClicked: Project.undo() }
        FlatButton { text: "Redo"; tip: "⇧⌘Z"; enabled: Project.canRedo; onClicked: Project.redo() }
        FlatButton {
            text: "Export"
            accent: true
            tip: "Export MP4 (⌘E)"
            leftPadding: 18
            rightPadding: 18
            Layout.leftMargin: 6
            enabled: Project.duration > 0 && !Exporter.running
            onClicked: bar.exportRequested()
        }
    }
}

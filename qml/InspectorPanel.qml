import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montage

Rectangle {
    id: panel
    readonly property var sel: Project.selection
    readonly property bool has: sel.id !== undefined

    color: Theme.panel

    function live(key, v) { Project.setSegmentProperty(sel.id, key, v, false) }
    function commit(key, v) { Project.setSegmentProperty(sel.id, key, v, true) }

    component SectionTitle: Label {
        color: Theme.text
        font.pixelSize: 12
        font.bold: true
        topPadding: 6
    }
    component InfoRow: RowLayout {
        property alias name: n.text
        property alias value: v.text
        Layout.fillWidth: true
        Label { id: n; color: Theme.textDim; font.pixelSize: 12; Layout.fillWidth: true }
        Label { id: v; color: Theme.text; font.pixelSize: 12; font.family: "Menlo" }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: col.implicitHeight + 28
        clip: true
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: col
            x: 14
            y: 14
            width: parent.width - 28
            spacing: 10

            Label {
                text: panel.has ? panel.sel.name : "Inspector"
                font.pixelSize: 14
                font.bold: true
                color: Theme.text
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
            Label {
                visible: !panel.has
                text: "Select a clip on the timeline to adjust it."
                color: Theme.textDim
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            ColumnLayout {
                visible: panel.has
                Layout.fillWidth: true
                spacing: 4
                InfoRow { name: "Track"; value: panel.has ? panel.sel.role : "" }
                InfoRow { name: "Starts at"; value: panel.has ? Project.formatTime(panel.sel.start) : "" }
                InfoRow { name: "Duration"; value: panel.has ? Project.formatTime(panel.sel.duration) : "" }
                InfoRow {
                    visible: panel.has && panel.sel.kind !== "image"
                    name: "Source in"
                    value: panel.has ? Project.formatTime(panel.sel.sourceStart) : ""
                }
            }

            ColumnLayout {
                visible: panel.has && panel.sel.kind !== "audio"
                Layout.fillWidth: true
                spacing: 6
                SectionTitle { text: "Picture" }
                PropertySlider {
                    label: "Scale"; unit: "%"; from: 5; to: 400; resetValue: 100
                    value: panel.has ? panel.sel.scale * 100 : 100
                    onMoved: (v) => panel.live("scale", v / 100)
                    onCommitted: (v) => panel.commit("scale", v / 100)
                }
                PropertySlider {
                    label: "Position X"; unit: "%"; from: -100; to: 100; resetValue: 0
                    value: panel.has ? panel.sel.x * 100 : 0
                    onMoved: (v) => panel.live("x", v / 100)
                    onCommitted: (v) => panel.commit("x", v / 100)
                }
                PropertySlider {
                    label: "Position Y"; unit: "%"; from: -100; to: 100; resetValue: 0
                    value: panel.has ? panel.sel.y * 100 : 0
                    onMoved: (v) => panel.live("y", v / 100)
                    onCommitted: (v) => panel.commit("y", v / 100)
                }
                PropertySlider {
                    label: "Opacity"; unit: "%"; from: 0; to: 100; resetValue: 100
                    value: panel.has ? panel.sel.opacity * 100 : 100
                    onMoved: (v) => panel.live("opacity", v / 100)
                    onCommitted: (v) => panel.commit("opacity", v / 100)
                }
                FlatButton {
                    text: "Reset picture"
                    Layout.alignment: Qt.AlignRight
                    onClicked: Project.resetTransform(panel.sel.id)
                }
            }

            ColumnLayout {
                visible: panel.has && panel.sel.hasAudio
                Layout.fillWidth: true
                spacing: 6
                SectionTitle { text: "Audio" }
                PropertySlider {
                    label: "Volume"; unit: "%"; from: 0; to: 200; resetValue: 100
                    value: panel.has ? panel.sel.volume * 100 : 100
                    onMoved: (v) => panel.live("volume", v / 100)
                    onCommitted: (v) => panel.commit("volume", v / 100)
                }
            }

            FlatButton {
                visible: panel.has
                text: "Delete clip"
                Layout.topMargin: 8
                onClicked: Project.deleteSelected()
            }
        }
    }
}

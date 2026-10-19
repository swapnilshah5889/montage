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
                visible: panel.has && panel.sel.kind === "text"
                Layout.fillWidth: true
                spacing: 8
                SectionTitle { text: "Text" }

                TextArea {
                    id: textEdit
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(64, contentHeight + 20)
                    wrapMode: TextEdit.Wrap
                    color: Theme.text
                    placeholderText: "Type your text"
                    background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: textEdit.activeFocus ? Theme.accent : Theme.border }
                    onEditingFinished: if (panel.has && text !== panel.sel.text) Project.setTextProperty(panel.sel.id, "text", text)
                    Binding on text {
                        when: !textEdit.activeFocus
                        value: panel.has && panel.sel.text !== undefined ? panel.sel.text : ""
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: Project.textPresets()
                        FlatButton {
                            required property string modelData
                            text: modelData
                            font.pixelSize: 11
                            background: Rectangle { implicitHeight: 26; radius: 5; color: parent.hovered ? "#34343a" : Theme.panelAlt }
                            onClicked: Project.applyTextPreset(panel.sel.id, modelData)
                        }
                    }
                }

                PropertySlider {
                    label: "Size"; unit: "%"; from: 2; to: 30; resetValue: 8
                    value: panel.has && panel.sel.textSize !== undefined ? panel.sel.textSize * 100 : 8
                    onMoved: (v) => Project.setTextProperty(panel.sel.id, "size", v / 100, false)
                    onCommitted: (v) => Project.setTextProperty(panel.sel.id, "size", v / 100, true)
                }

                Row {
                    spacing: 6
                    Repeater {
                        model: ["#FFFFFF", "#000000", "#1FD1C1", "#FFD60A", "#FF453A", "#0A84FF", "#FF2D92"]
                        Rectangle {
                            required property string modelData
                            width: 24; height: 24; radius: 12
                            color: modelData
                            border.width: panel.has && String(panel.sel.color).toUpperCase() === modelData ? 2 : 1
                            border.color: panel.has && String(panel.sel.color).toUpperCase() === modelData ? Theme.accent : Theme.border
                            TapHandler { onTapped: Project.setTextProperty(panel.sel.id, "color", parent.modelData) }
                        }
                    }
                }

                RowLayout {
                    spacing: 10
                    CheckBox {
                        text: "Bold"
                        focusPolicy: Qt.NoFocus
                        checked: panel.has && panel.sel.bold === true
                        onToggled: Project.setTextProperty(panel.sel.id, "bold", checked)
                    }
                    CheckBox {
                        text: "Outline"
                        focusPolicy: Qt.NoFocus
                        checked: panel.has && panel.sel.outline > 0
                        onToggled: Project.setTextProperty(panel.sel.id, "outline", checked ? 0.05 : 0)
                    }
                    CheckBox {
                        text: "Box"
                        focusPolicy: Qt.NoFocus
                        checked: panel.has && panel.sel.background !== undefined && panel.sel.background !== ""
                        onToggled: Project.setTextProperty(panel.sel.id, "background", checked ? "#B3000000" : "")
                    }
                }

                RowLayout {
                    spacing: 4
                    Label { text: "Align"; color: Theme.textDim; font.pixelSize: 12; Layout.fillWidth: true }
                    Repeater {
                        model: ["left", "center", "right"]
                        FlatButton {
                            required property string modelData
                            text: modelData === "left" ? "Left" : modelData === "center" ? "Center" : "Right"
                            font.pixelSize: 11
                            background: Rectangle {
                                implicitHeight: 26; radius: 5
                                color: panel.has && panel.sel.align === parent.modelData ? "#2f4f4c" : Theme.panelAlt
                            }
                            onClicked: Project.setTextProperty(panel.sel.id, "align", modelData)
                        }
                    }
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

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montage

Rectangle {
    id: panel
    readonly property var sel: Project.selection
    readonly property bool has: sel.id !== undefined

    color: Theme.panel

    // The playhead is passed so keyframed properties edit the key under it.
    function live(key, v) { Project.setSegmentProperty(sel.id, key, v, false, Player.position) }
    function commit(key, v) { Project.setSegmentProperty(sel.id, key, v, true, Player.position) }

    component MotionPicker: RowLayout {
        id: picker
        property string slot
        property string label
        property string current
        property real seconds: 0
        Layout.fillWidth: true
        spacing: 6
        Label { text: picker.label; color: Theme.textDim; font.pixelSize: 12; Layout.preferredWidth: 38 }
        ComboBox {
            Layout.fillWidth: true
            focusPolicy: Qt.NoFocus
            model: ["None"].concat(Project.motionNames(picker.slot))
            currentIndex: Math.max(0, model.indexOf(picker.current || "None"))
            onActivated: Project.setMotion(panel.sel.id, picker.slot, currentText === "None" ? "" : currentText, picker.seconds)
        }
        SpinBox {
            visible: picker.slot !== "loop" && picker.current !== ""
            focusPolicy: Qt.NoFocus
            from: 1; to: 50; stepSize: 1
            value: Math.round(picker.seconds * 10)
            textFromValue: (v) => (v / 10).toFixed(1) + "s"
            valueFromText: (t) => Math.round(parseFloat(t) * 10)
            implicitWidth: 92
            onValueModified: Project.setMotion(panel.sel.id, picker.slot, picker.current, value / 10)
        }
    }

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
                    spacing: 10
                    CheckBox {
                        text: "Shadow"
                        focusPolicy: Qt.NoFocus
                        checked: panel.has && !!panel.sel.shadow
                        onToggled: Project.setTextProperty(panel.sel.id, "shadow", checked ? "#000000" : "")
                    }
                    CheckBox {
                        text: "Gradient"
                        focusPolicy: Qt.NoFocus
                        checked: panel.has && !!panel.sel.gradient
                        onToggled: Project.setTextProperty(panel.sel.id, "gradient", checked ? "#FFD60A" : "")
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: panel.has && !!panel.sel.background
                    Label { text: "Box shape"; color: Theme.textDim; font.pixelSize: 12 }
                    ComboBox {
                        Layout.fillWidth: true
                        focusPolicy: Qt.NoFocus
                        model: Project.textShapes()
                        currentIndex: Math.max(0, model.indexOf(panel.has ? panel.sel.shape : "rounded"))
                        onActivated: Project.setTextProperty(panel.sel.id, "shape", currentText)
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
                visible: panel.has && panel.sel.kind !== "audio" && panel.sel.kind !== "effect"
                Layout.fillWidth: true
                spacing: 6
                SectionTitle { text: "Picture" }
                RowLayout {
                    visible: panel.has && panel.sel.onMain === true
                    Layout.fillWidth: true
                    spacing: 6
                    Label { text: "Frame"; color: Theme.textDim; font.pixelSize: 12; Layout.preferredWidth: 46
                            ToolTip.visible: frameHover.hovered
                            ToolTip.text: "Auto fills the frame and follows the subject; Fit shows the whole shot"
                            HoverHandler { id: frameHover } }
                    ComboBox {
                        Layout.fillWidth: true
                        focusPolicy: Qt.NoFocus
                        readonly property var labels: ({"auto": "Auto reframe", "fill": "Fill (centre)", "fit": "Fit", "fit-blur": "Fit + blurred fill"})
                        model: Project.framingNames().map(n => labels[n])
                        currentIndex: Math.max(0, Project.framingNames().indexOf(panel.has && panel.sel.framing ? panel.sel.framing : "auto"))
                        onActivated: (i) => Project.setFraming(panel.sel.id, Project.framingNames()[i])
                    }
                }
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
                    label: "Rotation"; unit: "°"; from: -180; to: 180; resetValue: 0
                    value: panel.has && panel.sel.rotation !== undefined ? panel.sel.rotation : 0
                    onMoved: (v) => panel.live("rotation", v)
                    onCommitted: (v) => panel.commit("rotation", v)
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
                visible: panel.has && panel.sel.kind !== "audio"
                Layout.fillWidth: true
                spacing: 6
                SectionTitle { text: panel.has && panel.sel.kind === "effect" ? "Effect layer" : "Effects" }
                Label {
                    visible: panel.has && panel.sel.kind === "effect"
                    text: "Applies to everything beneath it on the timeline."
                    color: Theme.textDim
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Repeater {
                    model: panel.has && panel.sel.effects ? panel.sel.effects : []
                    ColumnLayout {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        spacing: 0
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: modelData.name; color: Theme.text; font.pixelSize: 12; Layout.fillWidth: true }
                            Label { text: Math.round(modelData.amount * 100) + "%"; color: Theme.textDim; font.pixelSize: 11; font.family: "Menlo" }
                            FlatButton { text: "✕"; tip: "Remove"; padding: 2; onClicked: Project.removeEffect(panel.sel.id, index) }
                        }
                        Slider {
                            Layout.fillWidth: true
                            focusPolicy: Qt.NoFocus
                            from: 0; to: 2; value: modelData.amount
                            onMoved: Project.setEffectAmount(panel.sel.id, index, value, false)
                            onPressedChanged: if (!pressed) Project.setEffectAmount(panel.sel.id, index, value, true)
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        id: effectPicker
                        Layout.fillWidth: true
                        focusPolicy: Qt.NoFocus
                        model: panel.has && panel.sel.kind === "effect" ? Project.momentNames().concat(Project.effectNames()) : Project.effectNames()
                    }
                    FlatButton { text: "+ Add"; onClicked: Project.addEffect(panel.sel.id, effectPicker.currentText) }
                }
            }

            ColumnLayout {
                visible: panel.has && panel.sel.kind !== "audio" && panel.sel.kind !== "effect"
                Layout.fillWidth: true
                spacing: 6
                SectionTitle { text: "Animation" }
                MotionPicker { slot: "in"; label: "In"; current: panel.has ? panel.sel.motionIn : ""; seconds: panel.has ? panel.sel.motionInSec : 0.5 }
                MotionPicker { slot: "out"; label: "Out"; current: panel.has ? panel.sel.motionOut : ""; seconds: panel.has ? panel.sel.motionOutSec : 0.4 }
                MotionPicker { slot: "loop"; label: "Loop"; current: panel.has ? panel.sel.motionLoop : "" }
                RowLayout {
                    visible: panel.has && panel.sel.canTransition === true
                    Layout.fillWidth: true
                    spacing: 6
                    Label { text: "Cut"; color: Theme.textDim; font.pixelSize: 12; Layout.preferredWidth: 38
                            ToolTip.visible: cutHover.hovered; ToolTip.text: "Transition from the previous clip"
                            HoverHandler { id: cutHover } }
                    ComboBox {
                        Layout.fillWidth: true
                        focusPolicy: Qt.NoFocus
                        model: ["None"].concat(Project.transitionNames())
                        currentIndex: Math.max(0, model.indexOf(panel.has && panel.sel.transition ? panel.sel.transition : "None"))
                        onActivated: Project.setTransition(panel.sel.id, currentText === "None" ? "" : currentText, panel.has ? panel.sel.transitionSec : 0.5)
                    }
                    SpinBox {
                        visible: panel.has && !!panel.sel.transition
                        focusPolicy: Qt.NoFocus
                        from: 1; to: 30; stepSize: 1
                        value: panel.has ? Math.round(panel.sel.transitionSec * 10) : 5
                        textFromValue: (v) => (v / 10).toFixed(1) + "s"
                        valueFromText: (t) => Math.round(parseFloat(t) * 10)
                        implicitWidth: 92
                        onValueModified: Project.setTransition(panel.sel.id, panel.sel.transition, value / 10)
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: panel.has && panel.sel.keyframes.length > 0 ? panel.sel.keyframes.length + " keyframes" : "No keyframes"
                        color: Theme.textDim
                        font.pixelSize: 12
                        Layout.fillWidth: true
                    }
                    FlatButton {
                        text: "◆ Keyframe"
                        tip: "Record position, scale, rotation and opacity at the playhead"
                        enabled: panel.has && Player.position >= panel.sel.start && Player.position <= panel.sel.start + panel.sel.duration
                        onClicked: Project.addKeyframe(panel.sel.id, Player.position)
                    }
                    FlatButton {
                        text: "Clear"
                        visible: panel.has && panel.sel.keyframes.length > 0
                        onClicked: Project.clearKeyframes(panel.sel.id)
                    }
                }
            }

            ColumnLayout {
                visible: panel.has && panel.sel.kind === "video"
                Layout.fillWidth: true
                spacing: 6
                SectionTitle { text: "Speed" }
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: panel.has && panel.sel.speed === 0 ? "Freeze frame" : "Speed"; color: Theme.textDim; font.pixelSize: 12; Layout.fillWidth: true }
                    Label { text: panel.has ? (panel.sel.speed === 0 ? "hold" : panel.sel.speed.toFixed(2) + "×") : ""; color: Theme.text; font.pixelSize: 12; font.family: "Menlo" }
                }
                Slider {
                    // log scale: 0.1x .. 8x
                    id: speedSlider
                    visible: panel.has && panel.sel.speed > 0
                    Layout.fillWidth: true
                    focusPolicy: Qt.NoFocus
                    from: Math.log(0.1); to: Math.log(8)
                    value: Math.log(panel.has && panel.sel.speed > 0 ? panel.sel.speed : 1)
                    onPressedChanged: if (!pressed) Project.setSpeed(panel.sel.id, Math.exp(value), panel.sel.curve)
                }
                RowLayout {
                    visible: panel.has && panel.sel.speed > 0
                    Layout.fillWidth: true
                    Label { text: "Ramp"; color: Theme.textDim; font.pixelSize: 12 }
                    ComboBox {
                        Layout.fillWidth: true
                        focusPolicy: Qt.NoFocus
                        model: ["None"].concat(Project.speedCurves())
                        currentIndex: Math.max(0, model.indexOf(panel.has && panel.sel.curve ? panel.sel.curve : "None"))
                        onActivated: Project.setSpeed(panel.sel.id, panel.sel.speed, currentText === "None" ? "" : currentText)
                    }
                }
                FlatButton {
                    visible: panel.has && panel.sel.role === "main"
                    text: "❄ Freeze frame at playhead"
                    tip: "Hold the current frame for 2 seconds"
                    onClicked: Project.freezeFrame(Player.position, 2)
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
                CheckBox {
                    text: "Someone talking"
                    focusPolicy: Qt.NoFocus
                    checked: panel.has && panel.sel.speech === true
                    onToggled: Project.setSegmentProperty(panel.sel.id, "speech", checked ? 1 : 0)
                    ToolTip.visible: hovered
                    ToolTip.text: "Music set to dip will get quieter while this plays"
                }
                CheckBox {
                    text: "Dip while someone talks"
                    focusPolicy: Qt.NoFocus
                    checked: panel.has && panel.sel.duck > 0
                    onToggled: Project.setSegmentProperty(panel.sel.id, "duck", checked ? 0.3 : 0)
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

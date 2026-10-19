import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montage

ColumnLayout {
    id: root
    property string label
    property real from: 0
    property real to: 100
    property real value: 0
    property string unit: ""
    property real resetValue: NaN

    signal moved(real value)
    signal committed(real value)

    spacing: 0
    Layout.fillWidth: true

    RowLayout {
        Layout.fillWidth: true
        Label { text: root.label; color: Theme.textDim; font.pixelSize: 12; Layout.fillWidth: true }
        Label {
            text: Math.round(slider.value) + root.unit
            color: Theme.text
            font.pixelSize: 12
            font.family: "Menlo"
            TapHandler {
                enabled: !isNaN(root.resetValue)
                onDoubleTapped: root.committed(root.resetValue)
            }
        }
    }
    Slider {
        id: slider
        Layout.fillWidth: true
        focusPolicy: Qt.NoFocus
        from: root.from
        to: root.to
        value: root.value
        onMoved: root.moved(value)
        onPressedChanged: if (!pressed) root.committed(value)
    }
}

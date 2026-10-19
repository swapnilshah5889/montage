import QtQuick
import QtQuick.Controls
import Montage

Button {
    id: b
    property string tip: ""
    property bool accent: false

    flat: true
    focusPolicy: Qt.NoFocus
    padding: 6
    leftPadding: 10
    rightPadding: 10
    font.pixelSize: 13

    contentItem: Label {
        text: b.text
        font: b.font
        color: b.accent ? Theme.accentText : Theme.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        implicitHeight: 30
        implicitWidth: 30
        radius: 6
        opacity: b.enabled ? 1 : 0.4
        color: b.accent ? (b.down ? Qt.darker(Theme.accent, 1.25) : b.hovered ? Qt.lighter(Theme.accent, 1.1) : Theme.accent)
                        : (b.down ? "#3a3a40" : b.hovered ? "#2c2c31" : "transparent")
    }

    ToolTip.visible: tip !== "" && hovered
    ToolTip.text: tip
    ToolTip.delay: 600
}

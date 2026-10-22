import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montage

// First-run setup: the local AI pieces, whether they're here, and one click
// to get each one. Shown on first launch and from the ⚙ settings.
Popup {
    id: popup
    objectName: "setupDialog"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(620, parent ? parent.width - 80 : 620)
    padding: 24
    background: Rectangle { color: Theme.panel; radius: 12; border.color: Theme.border }
    onOpened: Setup.refresh()
    onClosed: Setup.markSeen()

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        Label { text: "Set up Montage's AI"; font.pixelSize: 18; font.bold: true; color: Theme.text }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: "Everything runs on this Mac: your footage never leaves it. The assistant needs Ollama and a model; "
                  + "the rest adds abilities and can be added any time."
        }

        Repeater {
            model: Setup.items
            delegate: Rectangle {
                id: row
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: content.implicitHeight + 20
                radius: 8
                color: Theme.panelAlt
                RowLayout {
                    id: content
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 12
                    Label {  // status mark
                        Layout.preferredWidth: 22
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: 16
                        text: row.modelData.state === "ok" ? "✓" : row.modelData.state === "working" ? "↓"
                              : row.modelData.state === "error" ? "!" : row.modelData.state === "optional" ? "○" : "●"
                        color: row.modelData.state === "ok" ? Theme.accent : row.modelData.state === "error" ? "#ff6b6b"
                              : row.modelData.state === "optional" ? Theme.textDim : "#f5b94a"
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        RowLayout {
                            spacing: 6
                            Label { text: row.modelData.name; color: Theme.text; font.bold: true }
                            Label {
                                visible: row.modelData.required && row.modelData.state !== "ok"
                                text: "needed"
                                font.pixelSize: 10
                                color: "#f5b94a"
                            }
                        }
                        Label { text: row.modelData.purpose; color: Theme.textDim; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                        Label { text: row.modelData.detail; color: Theme.textDim; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideRight }
                        ProgressBar {
                            Layout.fillWidth: true
                            visible: row.modelData.progress >= 0
                            value: Math.max(0, row.modelData.progress)
                        }
                    }
                    FlatButton {
                        visible: row.modelData.action !== ""
                        text: row.modelData.action
                        accent: row.modelData.action === "Download" || row.modelData.action === "Get Ollama"
                        onClicked: row.modelData.state === "working" ? Setup.cancel(row.modelData.id) : Setup.act(row.modelData.id)
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                color: Setup.ready ? Theme.accent : Theme.textDim
                font.pixelSize: 12
                text: Setup.ready ? "Ready: ask the assistant for a video." : "Install what's marked “needed” to use the assistant."
            }
            FlatButton { text: "Check again"; onClicked: Setup.refresh() }
            FlatButton { text: "Done"; accent: true; onClicked: popup.close() }
        }
    }
}

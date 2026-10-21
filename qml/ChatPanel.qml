import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montage

// The assistant: describe the video, the agent edits the project.
Rectangle {
    id: panel
    signal settingsRequested
    signal brandRequested
    signal importRequested

    color: Theme.panel

    function submit() {
        const text = input.text.trim()
        if (text === "" || Agent.busy) return
        Agent.send(text)
        input.text = ""
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header: model picker, settings, new chat
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12
            spacing: 6
            Label { text: "Assistant"; font.pixelSize: 14; font.bold: true; color: Theme.text }
            Item { Layout.fillWidth: true }
            ComboBox {
                id: modelBox
                focusPolicy: Qt.NoFocus
                // Shrinks before anything else so the header fits narrow panels.
                Layout.fillWidth: true
                Layout.preferredWidth: 170
                Layout.maximumWidth: 170
                Layout.minimumWidth: 70
                model: Agent.profiles
                textRole: "name"
                valueRole: "id"
                currentIndex: {
                    const list = Agent.profiles
                    for (let i = 0; i < list.length; ++i)
                        if (list[i].id === Agent.profileId) return i
                    return -1
                }
                onActivated: Agent.profileId = currentValue
                ToolTip.visible: hovered
                ToolTip.text: Agent.profile.model + (Agent.profile.guided ? " · guided mode" : " · agent mode")
            }
            FlatButton {
                text: "Brand"
                tip: Agent.brand.enabled ? "Brand kit (on)" : "Brand kit: colours, font, logo"
                accent: !!Agent.brand.enabled
                onClicked: panel.brandRequested()
            }
            FlatButton { text: "⚙"; tip: "Models and API keys"; font.pixelSize: 16; onClicked: panel.settingsRequested() }
            FlatButton { text: "New"; tip: "Start a new conversation"; enabled: list.count > 0; onClicked: Agent.newChat() }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: 12
                clip: true
                spacing: 8
                model: Agent.messages
                ScrollBar.vertical: ScrollBar {}
                onCountChanged: Qt.callLater(positionViewAtEnd)
                onContentHeightChanged: if (atYEnd || Agent.busy) Qt.callLater(positionViewAtEnd)

                delegate: Item {
                    id: row
                    required property string kind
                    required property string text
                    required property bool pending
                    required property real progress
                    required property string detail
                    required property string phase
                    width: ListView.view.width
                    height: kind === "generate" ? genCard.height : bubble.height

                    // A file being made by a local model: label, progress bar, what's happening.
                    Rectangle {
                        id: genCard
                        visible: row.kind === "generate"
                        width: parent.width
                        height: genColumn.implicitHeight + 18
                        radius: 10
                        color: Theme.panelAlt
                        border.color: row.phase === "failed" ? "#7a3a3e" : Theme.border

                        ColumnLayout {
                            id: genColumn
                            x: 12
                            y: 9
                            width: parent.width - 24
                            spacing: 6
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Label {
                                    text: row.phase === "done" ? "✓" : row.phase === "failed" ? "✕" : row.phase === "cancelled" ? "–" : "✦"
                                    color: row.phase === "done" ? Theme.accent : row.phase === "failed" ? "#ff6b6b" : Theme.textDim
                                    font.pixelSize: 13
                                    font.bold: true
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: row.text
                                    color: Theme.text
                                    font.pixelSize: 13
                                    elide: Text.ElideRight
                                }
                                Label {
                                    visible: row.phase === "running" && row.progress >= 0
                                    text: Math.round(row.progress * 100) + "%"
                                    color: Theme.textDim
                                    font.pixelSize: 11
                                }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                visible: row.phase === "running"
                                implicitHeight: 4
                                radius: 2
                                color: Theme.border
                                Rectangle {
                                    width: parent.width * Math.max(0, Math.min(1, row.progress))
                                    height: parent.height
                                    radius: 2
                                    color: Theme.accent
                                    Behavior on width { NumberAnimation { duration: 250 } }
                                }
                            }
                            TextEdit {
                                Layout.fillWidth: true
                                visible: row.detail !== ""
                                text: row.detail
                                readOnly: true
                                selectByMouse: true  // the "how to start ComfyUI" command can be copied
                                wrapMode: TextEdit.Wrap
                                textFormat: TextEdit.PlainText
                                color: row.phase === "failed" ? "#ff8a8a" : Theme.textDim
                                font.pixelSize: 11
                                selectionColor: Theme.accent
                            }
                        }
                    }

                    Rectangle {
                        id: bubble
                        visible: row.kind !== "generate"
                        readonly property bool isUser: row.kind === "user"
                        readonly property bool isNote: row.kind === "activity"
                        readonly property real maxWidth: row.width * (isUser ? 0.85 : 1)
                        anchors.right: isUser ? parent.right : undefined
                        anchors.left: isUser ? undefined : parent.left
                        width: isNote ? parent.width : body.width + 24
                        height: body.implicitHeight + (isNote ? 4 : 18)
                        radius: 10
                        color: isUser ? "#1d4f4b" : row.kind === "error" ? "#4a2124" : isNote ? "transparent" : Theme.panelAlt

                        TextEdit {
                            id: body
                            x: bubble.isNote ? 2 : 12
                            y: bubble.isNote ? 2 : 9
                            width: Math.min(implicitWidth, bubble.maxWidth - 24)
                            text: (bubble.isNote ? "•  " : "") + row.text + (row.pending ? " ▍" : "")
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.Wrap
                            textFormat: TextEdit.PlainText
                            color: bubble.isNote ? Theme.textDim : Theme.text
                            font.pixelSize: bubble.isNote ? 12 : 13
                            selectionColor: Theme.accent
                        }
                    }
                }
            }

            // Empty state with starter prompts: centred when it fits, scrolls when it doesn't.
            Flickable {
                id: welcome
                anchors.fill: parent
                visible: list.count === 0
                clip: true
                contentWidth: width
                contentHeight: Math.max(height, starters.implicitHeight + 32)
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            ColumnLayout {
                id: starters
                x: 24
                y: Math.max(16, (welcome.height - implicitHeight) / 2)
                width: welcome.width - 48
                spacing: 10
                Label {
                    Layout.fillWidth: true
                    text: "What do you want to make?"
                    font.pixelSize: 20
                    font.bold: true
                    color: Theme.text
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
                Label {
                    Layout.fillWidth: true
                    text: "Drop your clips, photos and music into the window (or mention a folder), then describe the video. The assistant edits the timeline for you, and you can still tweak anything by hand."
                    color: Theme.textDim
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    lineHeight: 1.25
                }
                FlatButton {
                    Layout.alignment: Qt.AlignHCenter
                    visible: Project.materials.length === 0
                    text: "+ Import media"
                    accent: true
                    onClicked: panel.importRequested()
                }
                Repeater {
                    model: [
                        "Make a 15 second vertical video from my clips with the music underneath",
                        "Make a 6 second intro that says \"Welcome to my channel\"",
                        "Make a 10 second ad for my candle shop with a cozy background image, a voiceover saying \"Light up your evenings\" and soft music",
                        "Add a title that says \"Summer Trip\" at the start",
                        "Cut it to the beat of the music and add captions",
                        "Export the video"
                    ]
                    Rectangle {
                        required property string modelData
                        Layout.fillWidth: true
                        implicitHeight: suggestion.implicitHeight + 16
                        radius: 8
                        color: tap.hovered ? "#2c2c31" : Theme.panelAlt
                        Label {
                            id: suggestion
                            anchors.fill: parent
                            anchors.margins: 8
                            text: parent.modelData
                            color: Theme.text
                            wrapMode: Text.WordWrap
                        }
                        HoverHandler { id: tap }
                        TapHandler { onTapped: { input.text = parent.modelData; input.forceActiveFocus() } }
                    }
                }
            }
            }
        }

        // Status while the agent works
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: Agent.busy
            spacing: 8
            BusyIndicator { running: Agent.busy; implicitWidth: 18; implicitHeight: 18 }
            Label { text: Agent.status; color: Theme.textDim; font.pixelSize: 12; Layout.fillWidth: true; elide: Text.ElideRight }
        }

        // Input
        Rectangle {
            Layout.fillWidth: true
            Layout.margins: 12
            implicitHeight: Math.min(160, Math.max(48, input.implicitHeight + 16))
            radius: 10
            color: Theme.panelAlt
            border.color: input.activeFocus ? Theme.accent : Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 6
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1  // take what's left, not the placeholder's width
                    TextArea {
                        id: input
                        objectName: "chatInput"
                        placeholderText: Agent.busy ? "Working…" : "Describe what you want…"
                        wrapMode: TextEdit.Wrap
                        color: Theme.text
                        background: null
                        Keys.onPressed: (e) => {
                            if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter) && !(e.modifiers & Qt.ShiftModifier)) {
                                e.accepted = true
                                panel.submit()
                            }
                        }
                    }
                }
                FlatButton {
                    Layout.alignment: Qt.AlignBottom
                    text: Agent.busy ? "Stop" : "Send"
                    accent: !Agent.busy
                    enabled: Agent.busy || input.text.trim() !== ""
                    onClicked: Agent.busy ? Agent.stop() : panel.submit()
                }
            }
        }
        Label {
            Layout.leftMargin: 14
            Layout.bottomMargin: 8
            text: Agent.profile.name + " · " + (Agent.profile.model || "no model chosen") + (Agent.profile.guided ? " · guided" : "")
            color: Theme.textDim
            font.pixelSize: 11
        }
    }
}

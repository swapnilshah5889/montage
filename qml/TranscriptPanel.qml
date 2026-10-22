import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montage

// Text-based editing: what's said on the main track, word by word. Click a
// word to jump there; shift-click to select a run of words and delete it to
// cut that part of the video. Fillers (um, uh) are struck through.
Rectangle {
    id: panel
    color: Theme.panel

    readonly property var words: Project.transcript
    property int selFrom: -1
    property int selTo: -1
    readonly property bool hasSelection: selFrom >= 0 && selTo >= selFrom
    readonly property int fillerCount: words.filter(w => w.filler).length
    readonly property int pauseCount: words.filter(w => w.pause >= 0.7).length
    // Paragraphs: a long pause, or a new clip after a pause, starts one. Each word keeps its index in `words`.
    readonly property var paragraphs: {
        const out = []
        let current = []
        for (let i = 0; i < words.length; ++i) {
            current.push({ word: words[i], index: i })
            const next = words[i + 1]
            if (!next || words[i].pause >= 1.0 || (next.segment !== words[i].segment && words[i].pause >= 0.5)) {
                out.push(current)
                current = []
            }
        }
        return out
    }

    function select(i, extend) {
        if (extend && selFrom >= 0) {
            selFrom = Math.min(selFrom, i)
            selTo = Math.max(selTo, i)
        } else {
            selFrom = selTo = i
        }
    }
    function cutSelection() {
        if (!hasSelection) return
        const start = words[selFrom].start
        // Up to the next word, so the pause after the last selected word goes too.
        const end = selTo + 1 < words.length ? words[selTo + 1].start - 30000 : words[selTo].end
        selFrom = selTo = -1
        Project.cutRange(start, end)
    }
    onWordsChanged: if (selTo >= words.length) selFrom = selTo = -1

    focus: true
    Keys.onPressed: (e) => {
        if ((e.key === Qt.Key_Backspace || e.key === Qt.Key_Delete) && hasSelection) {
            cutSelection()
            e.accepted = true
        } else if (e.key === Qt.Key_Escape) {
            selFrom = selTo = -1
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Label { text: "Transcript"; font.pixelSize: 14; font.bold: true; color: Theme.text }
            Item { Layout.fillWidth: true }
            Label { text: panel.words.length ? panel.words.length + " words" : ""; color: Theme.textDim; font.pixelSize: 11 }
        }
        Label {
            Layout.fillWidth: true
            visible: Agent.mediaStatus !== ""
            text: "◔ " + Agent.mediaStatus
            color: Theme.textDim
            font.pixelSize: 11
            elide: Text.ElideRight
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            visible: panel.words.length > 0
            FlatButton {
                text: "Remove fillers" + (panel.fillerCount ? " (" + panel.fillerCount + ")" : "")
                enabled: panel.fillerCount > 0
                tip: "Cut um, uh and similar"
                onClicked: Project.cleanSpeechNow(true, false)
            }
            FlatButton {
                text: "Tighten pauses" + (panel.pauseCount ? " (" + panel.pauseCount + ")" : "")
                enabled: panel.pauseCount > 0
                tip: "Shorten silences longer than 0.7 s"
                onClicked: Project.cleanSpeechNow(false, true)
            }
        }

        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            Column {
                id: paragraphsColumn
                width: scroll.availableWidth
                spacing: 10
                Repeater {
                    model: panel.paragraphs
                    delegate: Flow {
                        id: flow
                        required property var modelData
                        width: paragraphsColumn.width
                        spacing: 3
                        Repeater {
                            model: flow.modelData
                            delegate: Label {
                                id: chip
                                required property var modelData
                                readonly property var w: modelData.word
                                readonly property int index: modelData.index
                                readonly property bool current: Player.position >= w.start && Player.position < w.end
                                readonly property bool selected: panel.hasSelection && index >= panel.selFrom && index <= panel.selTo
                                text: w.text + (w.pause >= 0.7 ? "  ⏸ " + w.pause.toFixed(1) + "s" : "")
                                color: w.filler ? Theme.textDim : current ? Theme.accentText : Theme.text
                                font.pixelSize: 14
                                font.strikeout: w.filler
                                leftPadding: 2; rightPadding: 2; topPadding: 1; bottomPadding: 1
                                background: Rectangle {
                                    radius: 3
                                    color: chip.selected ? "#5a2a2a" : chip.current ? Theme.accent : hover.hovered ? Theme.panelAlt : "transparent"
                                }
                                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    acceptedModifiers: Qt.KeyboardModifierMask
                                    onTapped: (point) => {
                                        panel.forceActiveFocus()
                                        panel.select(chip.index, (point.modifiers & Qt.ShiftModifier) !== 0)
                                        Player.seek(chip.w.start)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: panel.hasSelection
            Label {
                Layout.fillWidth: true
                text: (panel.selTo - panel.selFrom + 1) + " word" + (panel.selTo > panel.selFrom ? "s" : "") + " selected"
                color: Theme.textDim
                font.pixelSize: 12
            }
            FlatButton { text: "Cut from video"; accent: true; tip: "Delete (⌫)"; onClicked: panel.cutSelection() }
        }

        Label {
            Layout.fillWidth: true
            visible: panel.words.length === 0
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            color: Theme.textDim
            text: "When a clip on the timeline has speech, its words show up here, transcribed on this Mac. "
                  + "Click a word to jump to it; shift-click to select words and cut them from the video."
        }
    }
}

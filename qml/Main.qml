import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Montage

ApplicationWindow {
    id: win
    width: 1440
    height: 900
    minimumWidth: 1000
    minimumHeight: 640
    visible: true
    color: Theme.bg
    title: (Project.dirty ? "● " : "") + Project.name + " — Montage"

    palette.window: Theme.panel
    palette.windowText: Theme.text
    palette.base: Theme.panelAlt
    palette.alternateBase: Theme.panel
    palette.text: Theme.text
    palette.button: Theme.panelAlt
    palette.buttonText: Theme.text
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.accentText
    palette.mid: Theme.border
    palette.dark: "#3a3a40"
    palette.light: "#4a4a52"
    palette.placeholderText: Theme.textDim
    palette.toolTipBase: Theme.panelAlt
    palette.toolTipText: Theme.text

    readonly property bool editingText: activeFocusItem instanceof TextInput || activeFocusItem instanceof TextEdit

    function save() {
        if (Project.fileUrl.toString() === "") saveDialog.open()
        else Project.save()
    }
    function showToast(text) {
        toastLabel.text = text
        toast.opacity = 1
        toastTimer.restart()
    }

    header: TopBar {
        onNewRequested: Project.newProject()
        onOpenRequested: openDialog.open()
        onSaveRequested: win.save()
        onExportRequested: exportOptions.open()
    }

    // Files dropped from Finder
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: (drop) => {
            if (!drop.hasUrls) return
            const urls = drop.urls
            drop.accept(Qt.CopyAction)
            Qt.callLater(() => {
                const n = Project.importFiles(urls)
                if (n > 0) win.showToast("Imported " + n + (n === 1 ? " file" : " files"))
            })
        }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Vertical
        handle: Rectangle {
            implicitHeight: 4
            color: SplitHandle.pressed || SplitHandle.hovered ? Theme.accent : Theme.bg
        }

        SplitView {
            SplitView.fillHeight: true
            SplitView.minimumHeight: 260
            orientation: Qt.Horizontal
            handle: Rectangle {
                implicitWidth: 4
                color: SplitHandle.pressed || SplitHandle.hovered ? Theme.accent : Theme.bg
            }

            ChatPanel {
                SplitView.preferredWidth: 400
                SplitView.minimumWidth: 300
                onSettingsRequested: modelSettings.open()
                onBrandRequested: brandSettings.open()
                onImportRequested: importDialog.open()
            }
            PlayerPanel {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 380
            }
            ColumnLayout {
                SplitView.preferredWidth: 300
                SplitView.minimumWidth: 230
                spacing: 0
                TabBar {
                    id: sideTabs
                    objectName: "sideTabs"
                    Layout.fillWidth: true
                    background: Rectangle { color: Theme.panel }
                    Repeater {
                        model: ["Media", "Edit", "Transcript"]
                        TabButton {
                            id: tab
                            required property string modelData
                            objectName: "tab" + modelData
                            text: modelData
                            focusPolicy: Qt.NoFocus
                            contentItem: Label {
                                text: tab.text
                                horizontalAlignment: Text.AlignHCenter
                                color: tab.checked ? Theme.text : Theme.textDim
                                font.bold: tab.checked
                            }
                            background: Rectangle {
                                implicitHeight: 36
                                color: tab.checked ? Theme.panel : Theme.bg
                                Rectangle {
                                    visible: tab.checked
                                    anchors.bottom: parent.bottom
                                    width: parent.width
                                    height: 2
                                    color: Theme.accent
                                }
                            }
                        }
                    }
                }
                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: sideTabs.currentIndex
                    MediaPanel { onImportRequested: importDialog.open() }
                    InspectorPanel {}
                    TranscriptPanel {}
                }
                Connections {
                    target: Project
                    function onSelectionChanged() { if (Project.selectedId !== "" && sideTabs.currentIndex !== 2) sideTabs.currentIndex = 1 }
                }
                Component.onCompleted: if (Project.selectedId !== "") sideTabs.currentIndex = 1
            }
        }

        TimelinePanel {
            SplitView.preferredHeight: 280
            SplitView.minimumHeight: 180
        }
    }

    Shortcut { sequence: "Space"; enabled: !win.editingText; onActivated: Player.toggle() }
    Shortcut { sequence: "Ctrl+B"; onActivated: Project.splitAt(Player.position) }
    Shortcut { sequences: [StandardKey.Delete, "Backspace"]; enabled: !win.editingText; onActivated: Project.deleteSelected() }
    Shortcut { sequences: [StandardKey.Undo]; enabled: !win.editingText; onActivated: Project.undo() }
    Shortcut { sequences: [StandardKey.Redo]; enabled: !win.editingText; onActivated: Project.redo() }
    Shortcut { sequences: [StandardKey.Save]; onActivated: win.save() }
    Shortcut { sequences: [StandardKey.Open]; onActivated: openDialog.open() }
    Shortcut { sequence: "Ctrl+I"; onActivated: importDialog.open() }
    Shortcut { sequence: "Ctrl+E"; enabled: Project.duration > 0; onActivated: exportDialog.open() }
    Shortcut { sequence: "Left"; enabled: !win.editingText; onActivated: Player.step(-1) }
    Shortcut { sequence: "Right"; enabled: !win.editingText; onActivated: Player.step(1) }
    Shortcut { sequence: "Home"; onActivated: Player.seek(0) }
    Shortcut { sequence: "Escape"; onActivated: Project.selectedId = "" }

    FileDialog {
        id: importDialog
        title: "Import media"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Media (*.mp4 *.mov *.m4v *.mkv *.webm *.avi *.mp3 *.wav *.m4a *.aac *.flac *.ogg *.png *.jpg *.jpeg *.webp *.heic)", "All files (*)"]
        onAccepted: {
            const n = Project.importFiles(selectedFiles)
            if (n > 0) win.showToast("Imported " + n + (n === 1 ? " file" : " files"))
        }
    }
    FileDialog {
        id: openDialog
        title: "Open project"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Montage projects (*.montage)"]
        onAccepted: Project.load(selectedFile)
    }
    FileDialog {
        id: saveDialog
        title: "Save project"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "montage"
        nameFilters: ["Montage projects (*.montage)"]
        onAccepted: if (Project.save(selectedFile)) win.showToast("Saved")
    }
    FileDialog {
        id: exportDialog
        property var formats: []
        title: formats.length > 1 ? "Export " + formats.length + " formats" : "Export video"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mp4"
        nameFilters: ["MPEG-4 video (*.mp4)"]
        onAccepted: {
            Player.pause()
            Exporter.start(selectedFile, formats)
        }
    }

    // Export: this shape, or several at once (one file each, reframed).
    Popup {
        id: exportOptions
        objectName: "exportOptions"
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 440
        padding: 22
        background: Rectangle { color: Theme.panel; radius: 10; border.color: Theme.border }
        property var picked: ({"9:16": true, "4:5": false, "1:1": true, "16:9": true})
        readonly property var shapes: [
            { name: "9:16", label: "Vertical", hint: "TikTok · Reels · Shorts · Stories" },
            { name: "4:5", label: "Portrait", hint: "Instagram & Facebook feed" },
            { name: "1:1", label: "Square", hint: "Feed posts · marketplaces" },
            { name: "16:9", label: "Landscape", hint: "YouTube · web · TV" }
        ]
        function chosen() {
            return shapes.map(s => s.name).filter(n => picked[n])
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            Label { text: "Export"; font.pixelSize: 16; font.bold: true; color: Theme.text }

            RadioButton {
                id: oneShape
                checked: true
                focusPolicy: Qt.NoFocus
                text: "This shape (" + Project.canvasWidth + "×" + Project.canvasHeight + ")"
            }
            RadioButton {
                id: manyShapes
                objectName: "exportManyShapes"
                focusPolicy: Qt.NoFocus
                text: "Every platform at once"
            }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 30
                visible: manyShapes.checked
                wrapMode: Text.WordWrap
                color: Theme.textDim
                font.pixelSize: 12
                text: "One file per shape. Clips are reframed around their subject, and text and logos move to fit."
            }
            Repeater {
                model: exportOptions.shapes
                delegate: RowLayout {
                    required property var modelData
                    Layout.leftMargin: 26
                    visible: manyShapes.checked
                    spacing: 8
                    CheckBox {
                        focusPolicy: Qt.NoFocus
                        checked: !!exportOptions.picked[modelData.name]
                        onToggled: {
                            const p = Object.assign({}, exportOptions.picked)
                            p[modelData.name] = checked
                            exportOptions.picked = p
                        }
                    }
                    Item {  // shape icon
                        implicitWidth: 22
                        implicitHeight: 22
                        Rectangle {
                            readonly property var wh: modelData.name.split(":").map(Number)
                            readonly property real k: 20 / Math.max(wh[0], wh[1])
                            anchors.centerIn: parent
                            width: wh[0] * k
                            height: wh[1] * k
                            color: "transparent"
                            border.color: Theme.accent
                            border.width: 1.5
                            radius: 2
                        }
                    }
                    Label { text: modelData.name; color: Theme.text; font.bold: true; Layout.preferredWidth: 38 }
                    Label { text: modelData.label + "  ·  " + modelData.hint; color: Theme.textDim; font.pixelSize: 12 }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                Item { Layout.fillWidth: true }
                FlatButton { text: "Cancel"; onClicked: exportOptions.close() }
                FlatButton {
                    text: manyShapes.checked ? "Export " + exportOptions.chosen().length + " files…" : "Export…"
                    accent: true
                    enabled: oneShape.checked || exportOptions.chosen().length > 0
                    onClicked: {
                        exportDialog.formats = manyShapes.checked ? exportOptions.chosen() : []
                        exportOptions.close()
                        exportDialog.open()
                    }
                }
            }
        }
    }

    ModelSettings { id: modelSettings }
    BrandSettings { id: brandSettings }

    Popup {
        id: exportPopup
        visible: Exporter.running
        modal: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: Overlay.overlay
        width: 380
        padding: 22
        background: Rectangle { color: Theme.panel; radius: 10; border.color: Theme.border }

        ColumnLayout {
            anchors.fill: parent
            spacing: 14
            Label { text: "Exporting video…"; font.pixelSize: 15; font.bold: true; color: Theme.text }
            Label { text: Exporter.stage; visible: Exporter.stage !== ""; color: Theme.textDim; font.pixelSize: 12 }
            ProgressBar { Layout.fillWidth: true; value: Exporter.progress }
            RowLayout {
                Layout.fillWidth: true
                Label { text: Math.round(Exporter.progress * 100) + "%"; color: Theme.textDim; font.family: "Menlo" }
                Item { Layout.fillWidth: true }
                FlatButton { text: "Cancel"; onClicked: Exporter.cancel() }
            }
        }
    }

    Rectangle {
        id: toast
        z: 1000
        opacity: 0
        visible: opacity > 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 28
        width: Math.min(toastLabel.implicitWidth + 36, win.width - 80)
        height: 40
        radius: 8
        color: "#2a2a30"
        border.color: Theme.border
        Label {
            id: toastLabel
            anchors.centerIn: parent
            width: parent.width - 36
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideMiddle
            color: Theme.text
        }
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Timer { id: toastTimer; interval: 4000; onTriggered: toast.opacity = 0 }
    }

    Connections {
        target: Project
        function onMessage(text) { win.showToast(text) }
    }
    Connections {
        target: Exporter
        function onFinished(ok, message, file, files) {
            if (ok && files.length > 1)
                win.showToast("Saved " + files.length + " formats next to " + decodeURIComponent(file.toString().replace("file://", "")).split("/").pop())
            else
                win.showToast(ok ? "Saved " + decodeURIComponent(file.toString().replace("file://", "")) : message)
        }
    }
}

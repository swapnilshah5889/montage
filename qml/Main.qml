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

    readonly property bool editingText: activeFocusItem instanceof TextInput

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
        onExportRequested: exportDialog.open()
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

            MediaPanel {
                SplitView.preferredWidth: 320
                SplitView.minimumWidth: 220
                onImportRequested: importDialog.open()
            }
            PlayerPanel {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 380
            }
            InspectorPanel {
                SplitView.preferredWidth: 290
                SplitView.minimumWidth: 230
            }
        }

        TimelinePanel {
            SplitView.preferredHeight: 340
            SplitView.minimumHeight: 200
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
        title: "Export video"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mp4"
        nameFilters: ["MPEG-4 video (*.mp4)"]
        onAccepted: {
            Player.pause()
            Exporter.start(selectedFile)
        }
    }

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
        function onFinished(ok, message, file) {
            win.showToast(ok ? "Saved " + decodeURIComponent(file.toString().replace("file://", "")) : message)
        }
    }
}

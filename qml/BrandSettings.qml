import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Montage

// Brand kit: colours, font, logo and house style the assistant applies to
// everything it makes (when on) or whenever a request mentions the brand.
Popup {
    id: popup
    objectName: "brandSettings"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 480
    padding: 22
    background: Rectangle { color: Theme.panel; radius: 12; border.color: Theme.border }

    property var form: ({})
    property string editing: ""  // colour key the colour dialog is editing
    onOpened: form = Object.assign({ text: "#FFFFFF", logoPosition: "top-right" }, Agent.brand)

    function set(key, value) {
        const f = Object.assign({}, form)
        f[key] = value
        form = f
    }

    ColorDialog {
        id: colorDialog
        onAccepted: popup.set(popup.editing, selectedColor.toString().toUpperCase())
    }
    FileDialog {
        id: logoDialog
        title: "Choose your logo"
        nameFilters: ["Images (*.png *.jpg *.jpeg *.webp *.svg)"]
        onAccepted: popup.set("logo", decodeURIComponent(selectedFile.toString().replace("file://", "")))
    }

    component Swatch: ColumnLayout {
        id: sw
        required property string key
        required property string label
        spacing: 4
        Rectangle {
            implicitWidth: 64
            implicitHeight: 40
            radius: 8
            color: popup.form[sw.key] || "transparent"
            border.color: Theme.border
            Label {
                anchors.centerIn: parent
                visible: !popup.form[sw.key]
                text: "+"
                color: Theme.textDim
                font.pixelSize: 18
            }
            TapHandler {
                onTapped: {
                    popup.editing = sw.key
                    colorDialog.selectedColor = popup.form[sw.key] || "#FF5A1F"
                    colorDialog.open()
                }
            }
        }
        Label { text: sw.label; color: Theme.textDim; font.pixelSize: 11; Layout.alignment: Qt.AlignHCenter }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Label { text: "Brand kit"; font.pixelSize: 16; font.bold: true; color: Theme.text; Layout.fillWidth: true }
            Switch {
                id: onSwitch
                focusPolicy: Qt.NoFocus
                checked: !!popup.form.enabled
                onToggled: popup.set("enabled", checked)
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 12
            text: onSwitch.checked ? "The assistant uses your brand on everything it makes."
                                   : "Off: the assistant uses your brand only when you ask (\"make it on-brand\")."
        }

        TextField {
            Layout.fillWidth: true
            placeholderText: "Brand name"
            text: popup.form.name || ""
            onTextEdited: popup.set("name", text)
        }

        RowLayout {
            spacing: 14
            Swatch { key: "primary"; label: "Primary" }
            Swatch { key: "accent"; label: "Accent" }
            Swatch { key: "text"; label: "Headlines" }
            Item { Layout.fillWidth: true }
        }

        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 10
            Layout.fillWidth: true

            Label { text: "Font"; color: Theme.textDim }
            ComboBox {
                Layout.fillWidth: true
                focusPolicy: Qt.NoFocus
                model: ["Default"].concat(Qt.fontFamilies())
                currentIndex: Math.max(0, model.indexOf(popup.form.font || "Default"))
                onActivated: popup.set("font", currentText === "Default" ? "" : currentText)
            }

            Label { text: "Style"; color: Theme.textDim }
            ComboBox {
                Layout.fillWidth: true
                focusPolicy: Qt.NoFocus
                model: ["Any"].concat(Agent.adStyles())
                currentIndex: Math.max(0, model.indexOf(popup.form.style || "Any"))
                onActivated: popup.set("style", currentText === "Any" ? "" : currentText)
            }

            Label { text: "Logo"; color: Theme.textDim }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Rectangle {
                    implicitWidth: 44
                    implicitHeight: 44
                    radius: 6
                    color: Theme.bg
                    border.color: Theme.border
                    Image {
                        anchors.fill: parent
                        anchors.margins: 4
                        fillMode: Image.PreserveAspectFit
                        source: popup.form.logo ? "file://" + popup.form.logo : ""
                        asynchronous: true
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: popup.form.logo ? popup.form.logo.split("/").pop() : "No logo"
                    color: popup.form.logo ? Theme.text : Theme.textDim
                    elide: Text.ElideMiddle
                }
                FlatButton { text: "Choose…"; onClicked: logoDialog.open() }
                FlatButton { text: "✕"; visible: !!popup.form.logo; tip: "Remove logo"; onClicked: popup.set("logo", "") }
            }

            Label { text: "Logo spot"; color: Theme.textDim }
            ComboBox {
                Layout.fillWidth: true
                focusPolicy: Qt.NoFocus
                model: ["top-right", "top-left", "bottom-right", "bottom-left"]
                currentIndex: Math.max(0, model.indexOf(popup.form.logoPosition || "top-right"))
                onActivated: popup.set("logoPosition", currentText)
            }
        }

        // What it will look like: a CTA button and a badge in the brand colours.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 74
            radius: 8
            color: "#0c0c0e"
            border.color: Theme.border
            RowLayout {
                anchors.centerIn: parent
                spacing: 14
                Label {
                    text: popup.form.name || "Your brand"
                    color: popup.form.text || "#FFFFFF"
                    font.family: popup.form.font || font.family
                    font.pixelSize: 20
                    font.bold: true
                }
                Rectangle {
                    implicitWidth: cta.implicitWidth + 28
                    implicitHeight: 34
                    radius: 17
                    color: popup.form.primary || Theme.accent
                    Label { id: cta; anchors.centerIn: parent; text: "Shop now"; font.bold: true; font.family: popup.form.font || font.family
                            color: Qt.colorEqual(popup.form.primary || Theme.accent, "#000000") ? "white" : ((Qt.color(popup.form.primary || Theme.accent).hslLightness > 0.6) ? "#111111" : "white") }
                }
                Rectangle {
                    implicitWidth: badge.implicitWidth + 20
                    implicitHeight: 28
                    radius: 6
                    color: popup.form.accent || popup.form.primary || Theme.accent
                    Label { id: badge; anchors.centerIn: parent; text: "30% OFF"; font.bold: true; font.family: popup.form.font || font.family
                            color: (Qt.color(popup.form.accent || popup.form.primary || Theme.accent).hslLightness > 0.6) ? "#111111" : "white" }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            FlatButton { text: "Cancel"; onClicked: popup.close() }
            FlatButton {
                text: "Save"
                accent: true
                onClicked: {
                    Agent.brand = popup.form
                    popup.close()
                }
            }
        }
    }
}

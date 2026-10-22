import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montage

// Model profiles: any OpenAI-compatible endpoint (OpenAI, DeepSeek,
// OpenRouter, Ollama, LM Studio, ...) or Claude. Keys go to the Keychain.
Popup {
    id: popup
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(820, parent ? parent.width - 80 : 820)
    height: Math.min(560, parent ? parent.height - 80 : 560)
    padding: 0
    background: Rectangle { color: Theme.panel; radius: 12; border.color: Theme.border }

    signal setupRequested
    property string selectedId: Agent.profileId
    property var form: ({})
    property var fetched: []
    property string fetchError: ""

    function load(id) {
        selectedId = id
        for (const p of Agent.profiles)
            if (p.id === id) form = Object.assign({}, p)
        keyField.text = ""
        fetched = []
        fetchError = ""
    }
    onOpened: load(Agent.profileId)

    Connections {
        target: Agent
        function onModelsFetched(id, models, error) {
            if (id !== popup.selectedId) return
            popup.fetched = models
            popup.fetchError = error
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Profile list
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 230
            color: Theme.bg
            radius: 12
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: "Models"; font.bold: true; font.pixelSize: 14; color: Theme.text; Layout.fillWidth: true }
                    FlatButton { text: "Set up…"; tip: "Check and download the local AI pieces"; onClicked: popup.setupRequested() }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    model: Agent.profiles
                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width
                        height: 44
                        radius: 6
                        color: modelData.id === popup.selectedId ? "#24403e" : hover.hovered ? Theme.panelAlt : "transparent"
                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            x: 10
                            Label { text: (modelData.id === Agent.profileId ? "● " : "") + modelData.name; color: Theme.text; font.pixelSize: 13 }
                            Label {
                                text: (modelData.model || "no model") + (modelData.local ? " · local" : modelData.hasKey ? " · key saved" : "")
                                color: Theme.textDim
                                font.pixelSize: 11
                            }
                        }
                        HoverHandler { id: hover }
                        TapHandler { onTapped: popup.load(parent.modelData.id) }
                    }
                }
                FlatButton {
                    text: "+ Add model"
                    Layout.fillWidth: true
                    onClicked: presetMenu.popup()
                    Menu {
                        id: presetMenu
                        Repeater {
                            model: Agent.presets()
                            MenuItem {
                                required property var modelData
                                text: modelData.name
                                onTriggered: {
                                    const p = Object.assign({}, modelData)
                                    delete p.preset
                                    p.id = ""
                                    popup.load(Agent.saveProfile(p))
                                }
                            }
                        }
                    }
                }
            }
        }

        // Form
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            GridLayout {
                width: parent.width - 40
                x: 20
                y: 18
                columns: 2
                columnSpacing: 12
                rowSpacing: 10

                Label { text: "Name"; color: Theme.textDim }
                TextField { Layout.fillWidth: true; text: popup.form.name || ""; onTextEdited: popup.form.name = text }

                Label { text: "Provider"; color: Theme.textDim }
                ComboBox {
                    Layout.fillWidth: true
                    model: [{ value: "openai", text: "OpenAI-compatible (OpenAI, DeepSeek, Ollama, LM Studio…)" },
                            { value: "anthropic", text: "Anthropic (Claude)" }]
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: indexOfValue(popup.form.kind || "openai")
                    onActivated: { popup.form.kind = currentValue; popup.formChanged() }
                }

                Label { text: "Base URL"; color: Theme.textDim }
                TextField {
                    Layout.fillWidth: true
                    text: popup.form.baseUrl || ""
                    placeholderText: "https://api.openai.com/v1"
                    onTextEdited: popup.form.baseUrl = text
                }

                Label { text: "Model"; color: Theme.textDim }
                RowLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        id: modelCombo
                        Layout.fillWidth: true
                        editable: true
                        model: popup.fetched
                        editText: popup.form.model || ""
                        onEditTextChanged: popup.form.model = editText
                        onActivated: popup.form.model = currentText
                    }
                    FlatButton {
                        text: "Fetch"
                        tip: "List the models this endpoint offers"
                        onClicked: {
                            Agent.saveProfile(popup.form)
                            if (keyField.text !== "") Agent.setApiKey(popup.selectedId, keyField.text)
                            Agent.fetchModels(popup.selectedId)
                        }
                    }
                }
                Item { implicitWidth: 1 }
                Label {
                    visible: popup.fetchError !== "" || popup.fetched.length > 0
                    text: popup.fetchError !== "" ? popup.fetchError : popup.fetched.length + " models available"
                    color: popup.fetchError !== "" ? "#ff7b7b" : Theme.textDim
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Label { text: "API key"; color: Theme.textDim }
                TextField {
                    id: keyField
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    placeholderText: popup.form.hasKey ? "Saved in Keychain (type to replace)" : popup.form.local ? "Not needed for local models" : "Paste your API key"
                }

                Label { text: "Can see images"; color: Theme.textDim }
                CheckBox {
                    checked: popup.form.vision === true
                    text: "Send frames to the model (vision models only)"
                    onToggled: popup.form.vision = checked
                }

                Label { text: "Mode"; color: Theme.textDim }
                ComboBox {
                    Layout.fillWidth: true
                    model: [{ value: "auto", text: "Auto (guided for local models)" },
                            { value: "agent", text: "Agent: model uses the editor's tools" },
                            { value: "guided", text: "Guided: one checked plan per request (small models)" }]
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: indexOfValue(popup.form.mode || "auto")
                    onActivated: popup.form.mode = currentValue
                }

                Label { text: "Effort"; color: Theme.textDim; visible: popup.form.kind === "anthropic" }
                ComboBox {
                    visible: popup.form.kind === "anthropic"
                    Layout.fillWidth: true
                    model: ["low", "medium", "high", "xhigh", "max"]
                    currentIndex: Math.max(0, model.indexOf(popup.form.effort || "medium"))
                    onActivated: popup.form.effort = currentText
                }

                Item { implicitHeight: 8; Layout.columnSpan: 2 }
                RowLayout {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    FlatButton {
                        text: "Delete"
                        enabled: Agent.profiles.length > 1
                        onClicked: { Agent.deleteProfile(popup.selectedId); popup.load(Agent.profileId) }
                    }
                    Item { Layout.fillWidth: true }
                    FlatButton { text: "Close"; onClicked: popup.close() }
                    FlatButton {
                        text: "Save & use"
                        accent: true
                        onClicked: {
                            const id = Agent.saveProfile(popup.form)
                            if (keyField.text !== "") Agent.setApiKey(id, keyField.text)
                            Agent.profileId = id
                            popup.load(id)
                            popup.close()
                        }
                    }
                }
            }
        }
    }
}

pragma Singleton
import QtQuick

QtObject {
    readonly property color bg: "#121214"
    readonly property color panel: "#1b1b1e"
    readonly property color panelAlt: "#242428"
    readonly property color border: "#2d2d32"
    readonly property color text: "#ececef"
    readonly property color textDim: "#8f8f98"
    readonly property color accent: "#1fd1c1"
    readonly property color accentText: "#06201d"
    readonly property color video: "#24617d"
    readonly property color image: "#6a4796"
    readonly property color audio: "#2b7148"
    readonly property color lane: "#18181b"
    readonly property color mainLane: "#1e2126"
    readonly property color playhead: "#ffffff"
    readonly property int radius: 6

    function kindColor(kind) {
        return kind === "audio" ? audio : kind === "image" ? image : video
    }
}

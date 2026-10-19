import QtQuick
import Montage

// Draws only the visible slice of the timeline, so it stays cheap at any
// zoom or project length.
Item {
    id: ruler
    property real pps: 60
    property real contentX: 0
    signal seek(real us)

    clip: true

    function step() {
        const steps = [0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800]
        for (const s of steps)
            if (s * pps >= 80) return s
        return 3600
    }
    function label(sec) {
        const m = Math.floor(sec / 60)
        const s = sec - m * 60
        const ss = s % 1 === 0 ? String(s).padStart(2, "0") : s.toFixed(1).padStart(4, "0")
        return String(m).padStart(2, "0") + ":" + ss
    }

    Rectangle { anchors.fill: parent; color: Theme.panel }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const major = ruler.step()
            const minor = major / 5
            const t0 = ruler.contentX / ruler.pps
            const t1 = (ruler.contentX + width) / ruler.pps
            ctx.font = "10px sans-serif"
            ctx.textBaseline = "top"
            for (let i = Math.floor(t0 / minor); i * minor <= t1; ++i) {
                const t = i * minor
                const x = Math.round(t * ruler.pps - ruler.contentX) + 0.5
                const isMajor = i % 5 === 0
                ctx.strokeStyle = isMajor ? "#77777f" : "#45454b"
                ctx.beginPath()
                ctx.moveTo(x, height)
                ctx.lineTo(x, height - (isMajor ? 10 : 5))
                ctx.stroke()
                if (isMajor) {
                    ctx.fillStyle = "#8f8f98"
                    ctx.fillText(ruler.label(Math.round(t * 10) / 10), x + 4, 4)
                }
            }
        }
    }
    onPpsChanged: canvas.requestPaint()
    onContentXChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()

    // Playhead marker
    Canvas {
        x: Player.position / 1e6 * ruler.pps - ruler.contentX - width / 2
        y: ruler.height - height
        width: 12
        height: 10
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.fillStyle = "#ffffff"
            ctx.beginPath()
            ctx.moveTo(0, 0)
            ctx.lineTo(width, 0)
            ctx.lineTo(width / 2, height)
            ctx.closePath()
            ctx.fill()
        }
    }

    MouseArea {
        anchors.fill: parent
        preventStealing: true
        onPressed: (m) => ruler.seek(Math.max(0, (m.x + ruler.contentX) / ruler.pps * 1e6))
        onPositionChanged: (m) => { if (pressed) ruler.seek(Math.max(0, (m.x + ruler.contentX) / ruler.pps * 1e6)) }
    }
}

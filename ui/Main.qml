import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import GoodSharing 1.0

ApplicationWindow {
    id: root
    visible: true
    width: 1280; height: 720
    minimumWidth: 640; minimumHeight: 360
    title: preview.shareActive ? "GoodSharing - sharing" : "GoodSharing"
    color: "#000000"
    font.family: "Segoe UI"
    font.pixelSize: 12

    palette.text: "#e8edf4"
    palette.window: "#0d1015"
    palette.windowText: "#e8edf4"
    palette.buttonText: "#e8edf4"
    palette.button: "#1b2029"
    palette.base: "#14181f"
    palette.alternateBase: "#10151c"
    palette.highlight: "#2f6db3"
    palette.highlightedText: "#ffffff"
    palette.mid: "#232a35"
    palette.midlight: "#1f2733"
    palette.light: "#2a3341"
    palette.dark: "#06080b"
    palette.shadow: "#000000"
    palette.brightText: "#000000"
    palette.placeholderText: "#7b8798"
    palette.toolTipBase: "#14181f"
    palette.toolTipText: "#e8edf4"
    palette.accent: "#2f6db3"

    property bool shareOpen: false
    property bool watchOpen: false
    property bool panelOpen: false
    property bool barVisible: true
    property int chrome: 28
    property color idle: "#9dabbd"
    property color hot: "#ffffff"
    property color muted: "#4f5a6a"

    // The application is the picture.
    RowLayout {
        anchors.fill: parent
        spacing: 1

        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: preview.watchBusy
            VideoSurface { objectName: "remoteVideoSurface"; anchors.fill: parent }
            MouseArea {
                id: tileMouse0
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: preview.focusSlot(0)
            }
            Rectangle {
                anchors { top: parent.top; right: parent.right; topMargin: root.chrome + 6; rightMargin: 8 }
                height: 22
                width: tagRow0.implicitWidth + 16
                radius: 11
                color: "#0b0d11"
                border.color: "#2a3341"
                visible: tileMouse0.containsMouse && preview.watchSlots.length > 0
                Row {
                    id: tagRow0
                    anchors.centerIn: parent
                    spacing: 10
                    Text {
                        text: preview.watchSlots.length > 0 ? preview.watchSlots[0].owner + " (sound)" : ""
                        color: "#dfe6ee"; font.pixelSize: 11
                    }
                    Text {
                        text: "x"
                        color: closeTile0.containsMouse ? "#ffffff" : "#c4ceda"
                        font.pixelSize: 12
                        MouseArea {
                            id: closeTile0
                            anchors.fill: parent; anchors.margins: -5
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: preview.stopWatchSlot(0)
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: preview.slotCount > 1
            VideoSurface { objectName: "remoteVideoSurface1"; anchors.fill: parent }
            MouseArea {
                id: tileMouse1
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: preview.focusSlot(1)
            }
            Rectangle {
                anchors { top: parent.top; right: parent.right; topMargin: root.chrome + 6; rightMargin: 8 }
                height: 22
                width: tagRow1.implicitWidth + 16
                radius: 11
                color: "#0b0d11"
                border.color: "#2a3341"
                visible: tileMouse1.containsMouse && preview.watchSlots.length > 1
                Row {
                    id: tagRow1
                    anchors.centerIn: parent
                    spacing: 10
                    Text {
                        text: preview.watchSlots.length > 1 ? preview.watchSlots[1].owner : ""
                        color: "#dfe6ee"; font.pixelSize: 11
                    }
                    Text {
                        text: "x"
                        color: closeTile1.containsMouse ? "#ffffff" : "#c4ceda"
                        font.pixelSize: 12
                        MouseArea {
                            id: closeTile1
                            anchors.fill: parent; anchors.margins: -5
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: preview.stopWatchSlot(1)
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: preview.slotCount > 2
            VideoSurface { objectName: "remoteVideoSurface2"; anchors.fill: parent }
            MouseArea {
                id: tileMouse2
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: preview.focusSlot(2)
            }
            Rectangle {
                anchors { top: parent.top; right: parent.right; topMargin: root.chrome + 6; rightMargin: 8 }
                height: 22
                width: tagRow2.implicitWidth + 16
                radius: 11
                color: "#0b0d11"
                border.color: "#2a3341"
                visible: tileMouse2.containsMouse && preview.watchSlots.length > 2
                Row {
                    id: tagRow2
                    anchors.centerIn: parent
                    spacing: 10
                    Text {
                        text: preview.watchSlots.length > 2 ? preview.watchSlots[2].owner : ""
                        color: "#dfe6ee"; font.pixelSize: 11
                    }
                    Text {
                        text: "x"
                        color: closeTile2.containsMouse ? "#ffffff" : "#c4ceda"
                        font.pixelSize: 12
                        MouseArea {
                            id: closeTile2
                            anchors.fill: parent; anchors.margins: -5
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: preview.stopWatchSlot(2)
                        }
                    }
                }
            }
        }
    }

    // Empty state.
    Column {
        anchors.centerIn: parent
        spacing: 6
        visible: preview.slotCount === 0 && preview.roomReady
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Nothing watched"
            color: "#8c99aa"; font.pixelSize: 16
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Open the people panel on the right, or use Watch in the top bar."
            color: "#5c6878"; font.pixelSize: 12
        }
    }

    // A minimized source keeps the share alive; say why the picture stopped.
    Rectangle {
        objectName: "remoteMinimizedNotice"
        anchors.fill: parent
        visible: preview.remoteSourceMinimized && preview.watchBusy
        color: "#000000"
        opacity: 0.94
        Column {
            anchors.centerIn: parent
            spacing: 8
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "User minimized app"
                color: "#e8edf4"; font.pixelSize: 20
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Waiting for them to restore it..."
                color: "#8c99aa"; font.pixelSize: 13
            }
        }
    }

    // Optional self-preview: the sharer's own capture as a small corner tile.
    Rectangle {
        id: selfPreviewTile
        objectName: "selfPreviewTile"
        anchors { right: parent.right; bottom: parent.bottom; margins: 12 }
        width: 320; height: 180
        radius: 4
        color: "#0b0d11"
        border.color: "#2a3341"
        visible: preview.shareActive && preview.selfPreview
        VideoSurface { objectName: "videoSurface"; anchors.fill: parent; anchors.margins: 1 }
        Text {
            anchors { left: parent.left; top: parent.top; margins: 6 }
            text: "Your preview"
            color: "#cfd8e3"; font.pixelSize: 10
        }
    }

    // Sharing notice with a stop action, independent of the auto-hiding bar.
    Rectangle {
        id: sharePill
        objectName: "sharePill"
        anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: root.chrome + 8 }
        height: 30
        width: pillRow.implicitWidth + 22
        radius: 15
        color: "#11151c"
        border.color: "#57d9a3"
        visible: preview.shareActive
        Row {
            id: pillRow
            anchors.centerIn: parent
            spacing: 10
            Rectangle {
                width: 8; height: 8; radius: 4
                anchors.verticalCenter: parent.verticalCenter
                color: "#57d9a3"
            }
            Text {
                text: "You are sharing"
                color: "#ffffff"; font.pixelSize: 13; font.weight: Font.DemiBold
            }
            Text {
                text: preview.sharingLabel.length > 0 ? preview.sharingLabel : ""
                color: "#aab6c6"; font.pixelSize: 12
                elide: Text.ElideRight
                width: Math.min(implicitWidth, 300)
            }
            Text {
                text: "Stop"
                color: stopPillMouse.containsMouse ? "#ffffff" : "#ff9a9a"
                font.pixelSize: 12
                MouseArea {
                    id: stopPillMouse
                    anchors.fill: parent; anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: preview.stopSharing()
                }
            }
        }
    }

    // Right-edge handle and the people panel it opens.
    Rectangle {
        id: railHandle
        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
        width: 20; height: 64
        radius: 6
        color: "#0b0d11"
        border.color: "#2a3341"
        visible: !root.panelOpen && preview.roomBusy
        Text {
            anchors.centerIn: parent
            text: "<"
            color: "#c4ceda"; font.pixelSize: 14
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.panelOpen = true
        }
    }

    Rectangle {
        id: peoplePanel
        objectName: "peoplePanel"
        anchors { top: parent.top; bottom: parent.bottom }
        width: 250
        x: root.panelOpen ? parent.width - width : parent.width
        visible: root.panelOpen
        color: "#0d1015"
        border.color: "#232a35"
        Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

        Text {
            id: panelClose
            anchors { top: parent.top; right: parent.right; topMargin: root.chrome + 8; rightMargin: 12 }
            text: ">"
            color: panelCloseMouse.containsMouse ? "#ffffff" : "#8c99aa"
            font.pixelSize: 15
            MouseArea {
                id: panelCloseMouse
                anchors.fill: parent; anchors.margins: -8
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.panelOpen = false
            }
        }

        Column {
            anchors { top: parent.top; topMargin: 36; left: parent.left; right: parent.right; bottom: parent.bottom }
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.bottomMargin: 10
            spacing: 2

            Text { text: "People"; color: "#e8edf4"; font.pixelSize: 12; font.weight: Font.DemiBold }

            Repeater {
                model: preview.participants
                Rectangle {
                    width: parent.width
                    height: 34
                    radius: 5
                    color: (rowMouse.containsMouse && modelData.sharing && !modelData.self) ? "#182130" : "transparent"
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: (modelData.sharing && !modelData.self) ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: {
                            if (modelData.sharing && !modelData.self) preview.watchOwner(modelData.identity)
                        }
                    }
                    Text {
                        anchors { left: parent.left; verticalCenter: parent.verticalCenter; leftMargin: 6 }
                        text: modelData.name
                        color: modelData.self ? "#8c99aa" : "#dfe6ee"
                        font.pixelSize: 12
                    }
                    Row {
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 6 }
                        spacing: 8
                        Item {
                            width: 58; height: 20
                            visible: modelData.sharing && !modelData.self && preview.watchSlots.length > 0
                                     && String(preview.watchSlots[0].owner) === String(modelData.identity)
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width; height: 3; radius: 1.5
                                color: "#232a35"
                            }
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width * preview.volume / 100
                                height: 3; radius: 1.5
                                color: "#5aa9ff"
                            }
                            Rectangle {
                                width: 9; height: 9; radius: 4.5
                                x: Math.max(0, Math.min(parent.width - 9, parent.width * preview.volume / 100 - 4.5))
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#e8edf4"
                                border.color: "#5aa9ff"
                            }
                            MouseArea {
                                anchors.fill: parent
                                preventStealing: true
                                onPressed: (mouse) => preview.setVolume(Math.round(100 * Math.max(0, Math.min(1, mouse.x / width))))
                                onPositionChanged: (mouse) => { if (pressed) preview.setVolume(Math.round(100 * Math.max(0, Math.min(1, mouse.x / width)))) }
                            }
                        }
                        Text {
                            text: modelData.self ? "this PC" : (modelData.sharing ? "sharing" : "")
                            color: modelData.self ? "#6c7a8c" : "#57d9a3"
                            font.pixelSize: 10
                        }
                    }
                }
            }

        }
    }

    // Join card: the only screen without a picture.
    Rectangle {
        id: joinPanel
        anchors.centerIn: parent
        width: 300
        height: joinColumn.implicitHeight + 20
        color: "#0d1015"
        border.color: "#232a35"
        visible: !preview.roomBusy
        ColumnLayout {
            id: joinColumn
            anchors.fill: parent
            anchors.margins: 10
            spacing: 6
            Text { text: "Join a room"; color: "#e8edf4"; font.pixelSize: 13 }
            TextField { id: joinUrl; placeholderText: "Room address"; Layout.fillWidth: true; font.pixelSize: 12 }
            TextField { id: joinToken; placeholderText: "Access token"; echoMode: TextInput.Password; Layout.fillWidth: true; font.pixelSize: 12 }
            TextField { id: joinKey; placeholderText: "Room key - 64 hex digits"; echoMode: TextInput.Password; Layout.fillWidth: true; font.pixelSize: 12 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Item { Layout.fillWidth: true }
                Text {
                    text: "Join"
                    color: joinMouse.containsMouse ? root.hot : root.idle
                    font.pixelSize: 12
                    MouseArea {
                        id: joinMouse
                        anchors.fill: parent; anchors.margins: -4
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            preview.joinRoom(joinUrl.text, joinToken.text, joinKey.text)
                            joinToken.clear(); joinKey.clear()
                        }
                    }
                }
            }
        }
    }

    // One thin, opaque bar: state, share, watch, sound, join/leave.
    Rectangle {
        id: bar
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: root.chrome
        visible: !preview.watchBusy || root.barVisible || root.shareOpen || root.watchOpen
        color: "#0b0d11"

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 6
            spacing: 12

            Rectangle {
                width: 6; height: 6; radius: 3
                Layout.alignment: Qt.AlignVCenter
                color: preview.shareActive ? "#57d9a3" : preview.watchBusy ? "#5aa9ff" : "#46505f"
            }
            Text {
                Layout.fillWidth: true
                elide: Text.ElideRight
                color: "#e8edf4"
                text: preview.shareActive
                      ? (preview.watchBusy ? "Sharing and watching" : (preview.sharingLabel.length > 0 ? "Sharing - " + preview.sharingLabel : "Sharing"))
                      : preview.watchBusy ? "Watching" : preview.roomStatus
            }
            Text {
                id: shareAction
                text: preview.shareActive ? "Stop share" : "Share"
                color: shareMouse.containsMouse ? root.hot : root.idle
                MouseArea {
                    id: shareMouse
                    anchors.fill: parent; anchors.margins: -4
                    hoverEnabled: true
                    onClicked: { root.watchOpen = false; root.shareOpen = !root.shareOpen }
                }
            }
            Text {
                id: watchAction
                text: preview.watchBusy ? "Stop watching" : "Watch"
                color: watchMouse.containsMouse ? root.hot : root.idle
                MouseArea {
                    id: watchMouse
                    anchors.fill: parent; anchors.margins: -4
                    hoverEnabled: true
                    onClicked: { root.shareOpen = false; root.watchOpen = !root.watchOpen }
                }
            }
            Text {
                id: roomAction
                text: preview.roomBusy ? "Leave" : "Join"
                color: roomMouse.containsMouse ? root.hot : root.idle
                MouseArea {
                    id: roomMouse
                    anchors.fill: parent; anchors.margins: -4
                    hoverEnabled: true
                    onClicked: {
                        root.shareOpen = false; root.watchOpen = false
                        if (preview.roomBusy) preview.leaveRoom()
                        else joinPanel.visible = !joinPanel.visible
                    }
                }
            }
        }

        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 1
            color: "#161b23"
            Rectangle {
                width: parent.width * Math.min(1, preview.audioLevel * 3)
                height: parent.height
                color: preview.shareActive ? "#57d9a3" : "#5aa9ff"
            }
        }
    }

    // Share popup.
    Popup {
        id: sharePopup
        parent: bar
        y: bar.height + 6
        x: parent.width - width - 6
        width: 330
        visible: root.shareOpen
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: root.shareOpen = false
        background: Rectangle { color: "#0d1015"; border.color: "#2a3341" }
        contentItem: ColumnLayout {
            spacing: 6

            RowLayout {
                Layout.fillWidth: true; spacing: 6
                Text { text: "Source"; color: "#8c99aa"; Layout.preferredWidth: 46 }
                ComboBox {
                    id: shareSource; Layout.fillWidth: true; font.pixelSize: 12
                    model: preview.sources; textRole: "label"
                    currentIndex: preview.selectedSourceIndex
                    enabled: !preview.shareActive
                }
                Text {
                    text: "R"
                    color: refreshMouse.containsMouse ? root.hot : root.idle
                    MouseArea {
                        id: refreshMouse
                        anchors.fill: parent; anchors.margins: -4
                        hoverEnabled: true
                        enabled: !preview.shareActive
                        onClicked: preview.refreshSources()
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 6
                Text { text: "Rate"; color: "#8c99aa"; Layout.preferredWidth: 46 }
                ComboBox {
                    id: shareFps; Layout.fillWidth: true; font.pixelSize: 12
                    model: ["30 fps", "60 fps"]
                    currentIndex: preview.shareFps === 30 ? 0 : 1
                    enabled: !preview.shareActive
                    onActivated: preview.setShareFps(currentIndex === 0 ? 30 : 60)
                }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 6
                Text { text: "Audio"; color: "#8c99aa"; Layout.preferredWidth: 46 }
                ComboBox {
                    id: shareAudio; Layout.fillWidth: true; font.pixelSize: 12
                    model: ["No audio", "Application", "Desktop"]
                    Component.onCompleted: currentIndex = preview.selectedAudioMode
                    enabled: !preview.shareActive
                }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 6
                visible: shareAudio.currentIndex === 1
                Text { text: "App"; color: "#8c99aa"; Layout.preferredWidth: 46 }
                ComboBox {
                    id: shareApp; Layout.fillWidth: true; font.pixelSize: 12
                    model: preview.applications; textRole: "label"
                    currentIndex: preview.selectedAudioApplication
                    enabled: !preview.shareActive
                }
            }
            CheckBox {
                text: "Show my preview while sharing"
                font.pixelSize: 12
                checked: preview.selfPreview
                onToggled: preview.setSelfPreview(checked)
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 6
                Text { text: preview.status; color: "#8c99aa"; elide: Text.ElideRight; Layout.fillWidth: true }
                Text {
                    text: preview.shareActive ? "Stop" : "Start"
                    color: (!preview.roomReady && !preview.shareActive) ? root.muted
                           : startMouse.containsMouse ? root.hot : root.idle
                    MouseArea {
                        id: startMouse
                        anchors.fill: parent; anchors.margins: -4
                        hoverEnabled: true
                        enabled: preview.roomReady || preview.shareActive
                        onClicked: preview.shareActive
                            ? preview.stopSharing()
                            : preview.startSharing(shareSource.currentIndex, shareAudio.currentIndex, shareApp.currentIndex)
                    }
                }
            }
        }
    }

    // Watch popup.
    Popup {
        id: watchPopup
        parent: bar
        y: bar.height + 6
        x: parent.width - width - 6
        width: 330
        visible: root.watchOpen
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: root.watchOpen = false
        background: Rectangle { color: "#0d1015"; border.color: "#2a3341" }
        contentItem: ColumnLayout {
            spacing: 6

            RowLayout {
                Layout.fillWidth: true; spacing: 6
                Text { text: "Share"; color: "#8c99aa"; Layout.preferredWidth: 46 }
                ComboBox {
                    id: watchTarget; Layout.fillWidth: true; font.pixelSize: 12
                    model: preview.remoteShares; textRole: "label"
                    enabled: preview.roomReady && count > 0
                }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 6
                Text { text: "Video"; color: "#8c99aa"; Layout.preferredWidth: 46 }
                ComboBox {
                    id: watchQuality; objectName: "watchQuality"; Layout.fillWidth: true; font.pixelSize: 12
                    model: ["720p", "1080p"]
                    enabled: preview.roomReady
                    Connections {
                        target: preview
                        function onWatchSelectionChanged() { watchQuality.currentIndex = preview.watchedQuality === 1080 ? 1 : 0 }
                    }
                }
                ComboBox {
                    id: watchProfile; objectName: "watchProfile"; Layout.preferredWidth: 120; font.pixelSize: 12
                    model: ["Low latency", "Balanced", "Smooth"]
                    currentIndex: preview.streamProfile
                    onActivated: preview.setStreamProfile(currentIndex)
                }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 6
                Text {
                    text: preview.remoteStatus
                    color: "#8c99aa"
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Layout.maximumWidth: 220
                }
                Text {
                    text: preview.watchBusy ? "Stop" : "Watch"
                    color: (!preview.roomReady || (watchTarget.count === 0 && !preview.watchBusy)) ? root.muted
                           : watchApplyMouse.containsMouse ? root.hot : root.idle
                    MouseArea {
                        id: watchApplyMouse
                        anchors.fill: parent; anchors.margins: -4
                        hoverEnabled: true
                        enabled: preview.roomReady && (preview.watchBusy || watchTarget.count > 0)
                        onClicked: preview.watchBusy
                            ? preview.stopWatching()
                            : preview.watchShare(watchTarget.currentIndex, watchQuality.currentIndex === 1 ? 1080 : 720)
                    }
                }
            }
        }
    }

    // While watching, the picture owns the screen: reveal the bar on hover.
    HoverHandler {
        onHoveredChanged: {
            root.barVisible = true
            hideTimer.restart()
        }
    }
    Timer {
        id: hideTimer
        interval: 2500
        onTriggered: if (preview.watchBusy && !root.shareOpen && !root.watchOpen && !root.panelOpen) root.barVisible = false
    }
    // Entering a watch hides the bar without waiting for a mouse move.
    Connections {
        target: preview
        function onStateChanged() {
            if (preview.watchBusy) hideTimer.restart()
        }
    }

    Shortcut {
        sequence: "Escape"
        onActivated: {
            root.shareOpen = false; root.watchOpen = false
            root.panelOpen = false
            joinPanel.visible = false
        }
    }
}

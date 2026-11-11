// 有可靠总量时显示真实累计进度；未知总量用忙碌指示，不映射成百分比。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: progress

    property string namePrefix: "operation"
    property bool busy: false
    property real value: -1
    property string statusText: ""
    property bool showRatio: false
    readonly property bool determinate: value >= 0

    objectName: namePrefix + "ProgressRow"
    visible: busy
    spacing: 4

    ProgressBar {
        id: progressBar
        objectName: progress.namePrefix + "ProgressBar"
        Layout.fillWidth: true
        visible: progress.determinate
        from: 0
        to: 1
        value: progress.value
        implicitHeight: 6

        background: Rectangle {
            radius: 3
            color: theme.hover
        }

        contentItem: Item {
            Rectangle {
                objectName: progress.namePrefix + "ProgressFill"
                width: progressBar.visualPosition * parent.width
                height: parent.height
                radius: 3
                color: theme.accent
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        BusyIndicator {
            objectName: progress.namePrefix + "BusyIndicator"
            visible: !progress.determinate
            running: !progress.determinate && progress.busy
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20
        }

        Text {
            objectName: progress.namePrefix + "ProgressText"
            Layout.fillWidth: true
            text: progress.statusText.length > 0
                  ? progress.statusText : "正在处理…"
            font.pixelSize: 14
            color: theme.textSecondary
            elide: Text.ElideRight
        }

        Text {
            objectName: progress.namePrefix + "ProgressRatioText"
            visible: progress.showRatio && progress.determinate
            text: Math.round(progressBar.position * 100) + "%"
            font.pixelSize: 14
            color: theme.textSecondary
        }
    }
}

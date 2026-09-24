// OperationPage.qml
//
// 备份页与恢复页共用的实现：结构与交互完全一样，只有文案和调用的控制器方法不同，
// 所以用一个 mode 参数区分，避免维护两份几乎逐行重复的 QML。

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import "../components"

Item {
    id: page

    property string mode: "backup"
    readonly property bool isBackup: mode === "backup"

    // 两个路径框共用一个目录选择器；targetField 记录这次是给哪个框选的。
    property int targetField: 0


    // 整页纵向滚动：内容超过窗口高度时页面本身可以滚（不是只给某个列表加滚轮）。
    ScrollView {
        id: pageScroll
        objectName: "operationPageScroll"
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        // 到顶/到底后继续滚不再被拉出去再弹回：ScrollView 的滚动主体是 Flickable，
        // 显式设成 StopAtBounds（默认是 DragAndOvershootBounds）。
        // contentItem 由样式在运行期提供、静态类型是 Item，所以只能运行期赋值，
        // 写成 contentItem.boundsBehavior: ... 这种静态绑定会被判成非法属性。
        Component.onCompleted: {
            if (pageScroll.contentItem)
                pageScroll.contentItem.boundsBehavior = Flickable.StopAtBounds
        }
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

    ColumnLayout {
        id: column
        x: Math.max(32, (pageScroll.availableWidth - width) / 2)
        y: 20
        width: Math.min(pageScroll.availableWidth - 64, 1400)
        spacing: 16

        Text {
            text: page.isBackup ? "备份" : "恢复"
            font.pixelSize: 30
            font.weight: Font.DemiBold
            color: theme.textPrimary
        }

        Text {
            text: page.isBackup ? "把一个目录打包成一个备份文件。" : "从备份文件恢复目录树。"
            font.pixelSize: 17
            color: theme.textSecondary
            Layout.topMargin: -8
        }

        AppCard {
            Layout.fillWidth: true
            Layout.topMargin: 4

            ColumnLayout {
                anchors.fill: parent
                spacing: 6

                Text {
                    text: page.isBackup ? "源目录" : "备份文件"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    color: theme.textSecondary
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    AppTextField {
                        Layout.fillWidth: true
                        enabled: !controller.busy
                        placeholderText: "可直接输入路径，也可以点击“浏览”"
                        text: page.isBackup ? controller.sourcePath : controller.backupFilePath
                        onTextEdited: {
                            if (page.isBackup)
                                controller.sourcePath = text
                            else
                                controller.backupFilePath = text
                        }
                    }

                    AppButton {
                        text: "浏览"
                        enabled: !controller.busy
                        onClicked: {
                            page.targetField = 0
                            if (page.isBackup) {
                                folderDialog.currentFolder = controller.directoryDialogStartUrl(controller.sourcePath)
                                folderDialog.open()
                            } else {
                                fileDialog.currentFile = controller.fileDialogStartUrl(controller.backupFilePath)
                                fileDialog.open()
                            }
                        }
                    }
                }

                Text {
                    text: page.isBackup ? "备份文件" : "恢复目录"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    color: theme.textSecondary
                    Layout.topMargin: 8
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    AppTextField {
                        Layout.fillWidth: true
                        enabled: !controller.busy
                        placeholderText: "可直接输入路径，也可以点击“浏览”"
                        text: page.isBackup ? controller.backupFilePath : controller.restorePath
                        onTextEdited: {
                            if (page.isBackup)
                                controller.backupFilePath = text
                            else
                                controller.restorePath = text
                        }
                    }

                    AppButton {
                        text: "浏览"
                        enabled: !controller.busy
                        onClicked: {
                            page.targetField = 1
                            if (page.isBackup) {
                                fileDialog.currentFile = controller.fileDialogStartUrl(controller.backupFilePath)
                                fileDialog.open()
                            } else {
                                folderDialog.currentFolder = controller.directoryDialogStartUrl(controller.restorePath)
                                folderDialog.open()
                            }
                        }
                    }
                }

        // 筛选规则交给可视化编辑器：QML 只收集表单值，
        // DSL、校验与匹配全部在 C++ 侧（FilterRuleModel + Filter）。
        FilterEditorPanel {
            ruleModel: filterRuleModel
            visible: page.isBackup
            Layout.fillWidth: true
            Layout.topMargin: 4
        }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 14
                    spacing: 12

                    AppButton {
                        text: page.isBackup ? "开始备份" : "开始恢复"
                        variant: "primary"
                        // busy 时禁用：整个程序只有一个控制器，天然保证同一时刻只有一个操作。
                        enabled: !controller.busy
                        onClicked: page.isBackup ? controller.startBackup() : controller.startRestore()
                    }

                    // 不确定进度条：核心没有百分比回调，这里只表达“在跑”。
                    Rectangle {
                        id: progressTrack
                        Layout.fillWidth: true
                        Layout.preferredHeight: 3
                        radius: 1.5
                        color: theme.hover
                        visible: controller.busy
                        clip: true

                        Rectangle {
                            id: progressChunk
                            width: progressTrack.width * 0.3
                            height: parent.height
                            radius: 1.5
                            color: theme.accent

                            NumberAnimation on x {
                                running: controller.busy
                                loops: Animation.Infinite
                                from: -progressChunk.width
                                to: progressTrack.width
                                duration: 1150
                                easing.type: Easing.InOutQuad
                            }
                        }
                    }
                }
            }
        }

        StatusBanner {
            Layout.fillWidth: true
            kind: controller.statusKind
            title: controller.statusTitle
            message: controller.statusMessage
        }

        Item { Layout.fillHeight: true }
    }
    }

    // 选择器只负责“帮忙填”：选完之后输入框仍然可以手改，
    // 因为备份文件允许是一个还不存在的路径，不能被选择器限制住。
    // 两个对话框都走这一个函数，省得把“填哪个字段”的分支写两遍。
    function applyChosenPath(path) {
        if (path === "")
            return
        if (targetField === 0) {
            if (isBackup)
                controller.sourcePath = path
            else
                controller.backupFilePath = path
        } else {
            if (isBackup)
                controller.backupFilePath = path
            else
                controller.restorePath = path
        }
        controller.clearStatus()
    }

    FolderDialog {
        id: folderDialog
        title: "选择目录"
        onAccepted: {
            // 转换交给 QUrl::toLocalFile()：中文、空格、# 与 % 都能原样还原；
            // 手写去掉 file:// 前缀会把 percent-encoding 留在路径里。
            page.applyChosenPath(controller.localPathFromUrl(folderDialog.selectedFolder))
        }
    }

    // 归档文件用文件对话框：备份页是“另存为”，恢复页是“打开”。
    // selectedFile 同样是 URL，转换复用同一个 helper。
    FileDialog {
        id: fileDialog
        title: "选择备份文件"
        fileMode: page.isBackup ? FileDialog.SaveFile : FileDialog.OpenFile
        nameFilters: ["Backup files (*.bak)", "All files (*)"]
        onAccepted: {
            page.applyChosenPath(controller.localPathFromUrl(fileDialog.selectedFile))
        }
    }
}

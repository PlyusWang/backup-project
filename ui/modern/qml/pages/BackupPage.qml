// BackupPage.qml
//
// 备份页。源目录 + 已配置的备份仓库，文件名由核心自动生成 ——
// 界面上不再有"备份文件完整路径"这个输入框，那是 repository-driven 之前
// 的用法，产品界面已经不需要它。

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import "../components"

Item {
    id: page

    // 页面不直接改 root.currentPage：跳转由 Main.qml 决定，页面之间保持解耦。
    signal openSettings()

    ScrollView {
        id: pageScroll
        objectName: "backupPageScroll"
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        // 到顶/到底后继续滚不再被拉出去再弹回：ScrollView 的滚动主体是 Flickable，
        // 显式设成 StopAtBounds（默认是 DragAndOvershootBounds）。
        // contentItem 由样式在运行期提供、静态类型是 Item，所以只能运行期赋值。
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
                text: "备份"
                font.pixelSize: 30
                font.weight: Font.DemiBold
                color: theme.textPrimary
            }

            Text {
                text: "选择一个目录，备份文件会自动命名并保存到备份仓库。"
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
                        text: "源目录"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        AppTextField {
                            objectName: "backupSourceField"
                            Layout.fillWidth: true
                            enabled: !controller.busy
                            placeholderText: "可直接输入路径，也可以点击“浏览”"
                            text: controller.sourcePath
                            onTextEdited: controller.sourcePath = text
                        }

                        AppButton {
                            text: "浏览"
                            enabled: !controller.busy
                            onClicked: {
                                sourceDialog.currentFolder = controller.directoryDialogStartUrl(controller.sourcePath)
                                sourceDialog.open()
                            }
                        }
                    }
                }
            }

            // 当前备份仓库：configured 时展示真实路径，unconfigured 时只给一条
            // 去设置页的入口，不在这一页偷偷替用户建目录。
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    Text {
                        text: controller.repositoryConfigured ? "当前备份仓库" : "尚未配置备份仓库"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    Text {
                        objectName: "backupRepositoryPath"
                        Layout.fillWidth: true
                        visible: controller.repositoryConfigured
                        text: controller.repositoryPath
                        font.pixelSize: 16
                        color: theme.textPrimary
                        wrapMode: Text.WrapAnywhere
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: controller.repositoryConfigured
                        text: "备份文件将自动命名并保存在该目录中。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    // 仓库配好之后同样要留一个修改入口：上面就是当前路径，
                    // 但"换仓库"统一发生在设置页 —— 这一页不直接编辑 repositoryPath，
                    // 也不复制 SettingsPage 的保存逻辑，否则同一套校验会有两份实现。
                    AppButton {
                        objectName: "changeRepositoryButton"
                        visible: controller.repositoryConfigured
                        text: "更改仓库"
                        iconName: "settings"
                        // 任务运行期间不给跳走，与"刷新"保持同一条禁用规则，
                        // 用户在"正在备份"时看到的两页状态是一致的。
                        enabled: !controller.busy
                        onClicked: page.openSettings()
                    }

                    AppButton {
                        objectName: "goToSettingsButton"
                        visible: !controller.repositoryConfigured
                        text: "前往设置"
                        iconName: "settings"
                        onClicked: page.openSettings()
                    }
                }
            }

            // 筛选规则交给可视化编辑器：QML 只收集表单值，
            // DSL、校验与匹配全部在 C++ 侧（FilterRuleModel + Filter）。
            FilterEditorPanel {
                ruleModel: filterRuleModel
                Layout.fillWidth: true
                Layout.topMargin: 4
            }

            // 高级选项（打包格式 / 压缩 / 加密 + 密码）：默认收起。
            // 默认取值（mypack + 不压缩 + 不加密）与之前完全一致，
            // 不展开就不会碰到新选项，默认产物也不会因为这一块而改变。
            BackupOptionsPanel {
                id: panel
                Layout.fillWidth: true
                Layout.topMargin: 4
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 14
                spacing: 12

                AppButton {
                    objectName: "startBackupButton"
                    text: "开始备份"
                    variant: "primary"
                    // busy 时禁用：整个程序只有一个控制器，天然保证同一时刻只有一个操作。
                    // 密码为空或两次不一致时不再直接把按钮禁用 —— 那样用户根本没有
                    // "再点一次提交、然后才看到错误"的机会。校验改成点击时请求：
                    // 面板先亮出错误，合法才真的调用控制器。
                    // 规则仍然只有那两条，判定也仍然只写在面板里。
                    enabled: !controller.busy
                    // 三个算法一律传冻结的字符串键；密码与确认密码原样交给控制器，
                    // 界面不在这里做任何加工（不加盐、不截断、不拼进任何路径）。
                    //
                    // 顺序是刻意的：先请求校验 -> 不合法就 return（完全不碰控制器，
                    // 错误提示由面板自己显示）-> 合法才提交。只有控制器真的收下了
                    // 这次任务（返回 true —— 此时 Start() 已经把 OperationRequest
                    // 的值拷贝交给 QtConcurrent）才清空两个密码框并收起提示。
                    // 控制器同步失败时（没选源目录、没配仓库、未知 key）密码保留，
                    // 用户改完可以直接再点一次；这里也不动 encryptionKey。
                    onClicked: {
                        panel.requestPasswordValidation()

                        if (!panel.passwordAcceptable)
                            return

                        const started = controller.startBackupWithOptions(
                            panel.packKey,
                            panel.compressionKey,
                            panel.encryptionKey,
                            panel.password,
                            panel.confirmPassword)

                        if (started)
                            panel.clearPasswords()
                    }
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

            StatusBanner {
                Layout.fillWidth: true
                kind: controller.statusKind
                title: controller.statusTitle
                message: controller.statusMessage
            }

            Item { Layout.fillHeight: true }
        }
    }

    FolderDialog {
        id: sourceDialog
        objectName: "backupSourceDialog"
        title: "选择源目录"
        onAccepted: {
            // 转换交给 QUrl::toLocalFile()：中文、空格、# 与 % 都能原样还原；
            // 手写去掉 file:// 前缀会把 percent-encoding 留在路径里。
            const chosen = controller.localPathFromUrl(sourceDialog.selectedFolder)
            if (chosen !== "")
                controller.sourcePath = chosen
        }
    }
}

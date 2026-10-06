// SettingsPage.qml
//
// 设置页。目前只有一个真实存在的设置：备份仓库。
// 压缩 / 加密 / 调度 / 实时备份都还没有实现，所以这里不画它们的开关 ——
// 界面上的每一项都必须是能真正生效的东西。
// 边界：这一页只写 config.json 里的一项（backup_repository_path），不读归档、
// 不碰 schedule store、不发起任何备份。保存与 `config repository set` 走的是
// 同一个控制器方法，所以 CLI 与 GUI 的写入顺序（先建目录、再落盘）完全一致。

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import "../components"

Item {
    id: page

    // draft 语义：输入框里的是草稿，只有点"保存设置"才会写进控制器。
    // 用户每敲一个字符就改 controller.repositoryPath 会让"当前保存值"
    // 和"正在编辑的值"混在一起，也会让配置在没确认的情况下就被刷新。
    // 不变量：draftPath 只会被"首帧 / 保存值变化 / 用户输入"改写；保存失败时
    // 控制器不动 repositoryPath，所以草稿不会被后端回滚，用户不用重敲一遍。
    property string draftPath: ""

    // 真实保存值：永远跟着控制器走。这一条是绑定，不会被用户输入打断
    // （用户改的是 draftPath，不是它）。
    // 只读投影：它是控制器当前的真值，不是编辑目标；写它不会回写控制器
    // （这条绑定是单向的），用户输入因此只落在 draftPath 上。
    property string savedPath: controller.repositoryPath

    // 草稿的同步点只有这两处：首帧，以及真实值变化时（保存成功、或从别处改了
    // 仓库）。用属性变化处理函数而不是 Connections，少一个需要解析的类型，
    // 也不会出现"target 写错却静默不生效"的情况。
    Component.onCompleted: draftPath = controller.repositoryPath
    onSavedPathChanged: draftPath = controller.repositoryPath

    ScrollView {
        id: pageScroll
        objectName: "settingsPageScroll"
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        // contentItem 要等这一帧结束才存在，所以 boundsBehavior 只能在这里设；
        // StopAtBounds 关掉回弹，内容比视口短时页面也不会被拖着晃。
        Component.onCompleted: {
            if (pageScroll.contentItem)
                pageScroll.contentItem.boundsBehavior = Flickable.StopAtBounds
        }
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            id: column
            // 版心：左右至少留 32 的边距，内容最宽 1400。超宽屏上不让一行文字
            // 拉到难以阅读的长度，窄窗口上靠 Math.max 保证边距不被吃掉。
            x: Math.max(32, (pageScroll.availableWidth - width) / 2)
            y: 20
            width: Math.min(pageScroll.availableWidth - 64, 1400)
            spacing: 16

            Text {
                text: "设置"
                font.pixelSize: 30
                font.weight: Font.DemiBold
                color: theme.textPrimary
            }

            Text {
                text: "配置备份仓库。备份文件会自动命名并保存在该目录中。"
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
                        text: "备份仓库"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: theme.textSecondary
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        AppTextField {
                            id: repositoryField
                            objectName: "repositoryPathField"
                            Layout.fillWidth: true
                            // 保存期间锁住输入：保存是异步的，改草稿会让
                            // 提交的值与显示的值不一致，用户会以为没生效。
                            enabled: !controller.busy
                            placeholderText: "输入目录路径，" +
                                             "或点击“浏览目录”选择"
                            text: page.draftPath
                            // 只有用户输入会写草稿：程序化回填不走这个信号，
                            // 所以首帧与保存成功后的赋值不会把草稿再改一遍。
                            onTextEdited: page.draftPath = text
                        }

                        AppButton {
                            objectName: "browseRepositoryButton"
                            text: "浏览目录"
                            iconName: "folder"
                            enabled: !controller.busy
                            onClicked: {
                                repositoryDialog.currentFolder =
                                    controller.directoryDialogStartUrl(
                                        page.draftPath)
                                repositoryDialog.open()
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "目录不存在时会自动创建。" +
                              "手写路径不会被选择器限制。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                    }

                    Text {
                        // objectName 是自动化 UI 检查的定位契约（按它找控件），
                        // 改名等于改测试接口 —— 不要顺手改。
                        objectName: "savedRepositoryText"
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        text: controller.repositoryConfigured
                              ? "当前保存的备份仓库："
                                + controller.repositoryPath
                              : "尚未保存备份仓库。"
                        font.pixelSize: 15
                        color: theme.textSecondary
                        wrapMode: Text.WrapAnywhere
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 10
                        spacing: 12

                        AppButton {
                            objectName: "saveRepositoryButton"
                            text: "保存设置"
                            variant: "primary"
                            enabled: !controller.busy
                            onClicked:
                                controller.saveRepositoryPath(
                                    page.draftPath)
                        }

                        Item { Layout.fillWidth: true }
                    }
                }
            }

            StatusBanner {
                objectName: "settingsStatusBanner"
                Layout.fillWidth: true
                // 状态横幅只消费本页产生的状态：切页后控制器里残留的备份 /
                // 恢复提示不会串到这里来。
                pageScope: "settings"
                scope: controller.statusScope
                kind: controller.statusKind
                title: controller.statusTitle
                message: controller.statusMessage
            }

            Item { Layout.fillHeight: true }
        }
    }

    // 选择器只负责"帮忙填"：选完之后草稿仍然可以手改，
    // 因为保存时 EnsureRepository 会创建不存在的目录。
    FolderDialog {
        id: repositoryDialog
        objectName: "repositoryFolderDialog"
        title: "选择备份仓库目录"
        onAccepted: {
            const chosen =
                controller.localPathFromUrl(repositoryDialog.selectedFolder)
            if (chosen !== "")
                page.draftPath = chosen
        }
    }
}

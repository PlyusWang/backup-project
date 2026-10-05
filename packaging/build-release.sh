#!/usr/bin/env bash
# build-release.sh —— 从**一个干净的 commit** 生成两个产品族的全部发行制品。
#
#   bash packaging/build-release.sh --version <semver> [--out <目录>] [--only client|server]
#                                    [--skip-appimage] [--skip-stage]
#
# 产出（默认 dist/release/<版本>/）：
#   Backup-Project-Client-<版本>-x86_64.AppImage
#   backup-project-client_<版本>_amd64.deb
#   backup-project-client-<版本>-linux-x86_64.tar.xz
#   backup-project-server_<版本>_amd64.deb
#   backup-project-server-<版本>-linux-x86_64.tar.xz
#   SHA256SUMS、RELEASE-INFO.txt
#
# 三件必须成立的性质：
#   1. 可复现：SOURCE_DATE_EPOCH = commit 时间，固定 TZ/LC_ALL，归档排序与 mtime
#      固定，owner/group 固定，deb/tar 的字节内容只由 commit 决定；
#   2. 不夹带私钥：制品里绝不允许出现根私钥、transport.key、secrets.env、口令；
#   3. 包装而不是重写：内容来自 dist/client 与 dist/server（scripts/stage-*-release.sh
#      是这一层的唯一真值来源），打包层只加启动器/配置/unit/安装脚本。
#
# 本脚本**不创建 tag、不发布 Release**：版本号是参数，发布是人的决定。

set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

usage() { sed -n '2,26p' "$0"; }

VERSION=""
OUT_ROOT="$REPO_ROOT/dist/release"
ONLY="all"
SKIP_APPIMAGE=0
SKIP_STAGE=0
JOBS="$(nproc 2>/dev/null || echo 4)"

while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION="${2:-}"; shift 2 ;;
    --out) OUT_ROOT="${2:-}"; shift 2 ;;
    --only) ONLY="${2:-}"; shift 2 ;;
    --skip-appimage) SKIP_APPIMAGE=1; shift ;;
    --skip-stage) SKIP_STAGE=1; shift ;;
    --jobs) JOBS="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die_usage "未知参数：$1" ;;
  esac
done

case "$ONLY" in all|client|server) ;; *) die_usage "--only 只接受 all / client / server" ;; esac

cd "$REPO_ROOT"
require_clean_tree
require_tool make g++ dpkg-deb tar xz sha256sum readelf awk stat file

# 打包层依赖的文件必须先齐全：.gitignore 里有一条 "backupctl"（忽略构建产物），
# 曾经把 packaging/client/wrappers/backupctl 静默吞掉，直到 CI 里 install 才报错。
# 这里提前失败，并且把"少了哪个文件"直接说出来。
for needed in \
  packaging/lib/common.sh packaging/fetch-tools.sh packaging/tools.lock \
  packaging/client/AppRun packaging/client/AppImage.desktop packaging/client/backup-project.desktop \
  packaging/client/make-icon.py packaging/client/wrappers/backupctl.sh packaging/client/wrappers/backup-project \
  packaging/server/server.conf packaging/server/launch-server.sh packaging/server/purge-data.sh \
  packaging/server/tmpfiles.conf \
  packaging/server/backup-project-server.service packaging/server/deb/control.in \
  packaging/server/deb/postinst packaging/server/deb/prerm packaging/server/deb/postrm \
  packaging/server/wrappers/backup-server packaging/server/wrappers/backup-server-admin \
  packaging/server/wrappers/backup-server-admin-menu \
  packaging/server/wrappers/backup-server-keygen packaging/server/wrappers/backup-cert-tool \
  packaging/server/wrappers/backup-server-purge-data \
  packaging/portable/install-client.sh packaging/portable/uninstall-client.sh \
  packaging/portable/install-server.sh packaging/portable/uninstall.sh; do
  [ -f "$needed" ] || die "打包层缺少文件：$needed（是不是被 .gitignore 吞了？）"
done
setup_reproducible_env
[ -n "$VERSION" ] || VERSION="$(default_dev_version)"
VERSION="$(validate_version "$VERSION")"
DEB_VERSION="$(deb_version_of "$VERSION")"
ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"

RELEASE_DIR="$OUT_ROOT/$VERSION"
WORK="$REPO_ROOT/dist/.packaging-work"
rm -rf "$WORK" "$RELEASE_DIR"
mkdir -p "$WORK" "$RELEASE_DIR"
log "版本 $VERSION（deb: $DEB_VERSION）架构 $ARCH"
log "制品目录：$RELEASE_DIR"

OS_PRETTY="$( . /etc/os-release 2>/dev/null && echo "$PRETTY_NAME" || echo unknown )"
GLIBC_VERSION="$(ldd --version | awk 'NR==1 {print $NF}')"
CXX_VERSION="$(g++ --version | awk 'NR==1')"
QT_VERSION="$(command -v qmake6 >/dev/null 2>&1 && qmake6 -query QT_VERSION || echo none)"
COMMIT="$(git rev-parse HEAD)"
TREE="$(git rev-parse HEAD^{tree})"
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
STAMP="$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ)"

write_build_info() {
  local dir="$1" product="$2"
  mkdir -p "$dir"
  cat > "$dir/BUILD-INFO.txt" <<EOF
product           = $product
version           = $VERSION
deb_version       = $DEB_VERSION
commit            = $COMMIT
tree              = $TREE
branch            = $BRANCH
working_tree      = clean
build_os          = $OS_PRETTY
build_glibc       = $GLIBC_VERSION
compiler          = $CXX_VERSION
qt_version        = $QT_VERSION
architecture      = $ARCH
source_date_epoch = $SOURCE_DATE_EPOCH
build_timestamp   = $STAMP
EOF
}

# ---- deb：用 dpkg-shlibdeps 算依赖，不手写拍脑袋的库列表 ----
shlibs_depends() {
  local helper="$WORK/debhelper"
  mkdir -p "$helper/debian"
  cat > "$helper/debian/control" <<'EOF'
Source: backup-project
Section: admin
Priority: optional
Maintainer: Backup Project <noreply@example.invalid>
Standards-Version: 4.5.0

Package: backup-project-shlibs-helper
Architecture: any
Description: helper package used only to run dpkg-shlibdeps
EOF
  ( cd "$helper" && dpkg-shlibdeps -O "$@" 2>/dev/null | sed -n 's/^shlibs:Depends=//p' )
}

build_deb() {  # $1=deb_dir $2=tree $3=out $4=depends
  local deb_dir="$1" tree="$2" out="$3" depends="$4"
  local root="$WORK/debroot"
  rm -rf "$root"; mkdir -p "$root/DEBIAN"
  ( cd "$tree" && tar -cf - . ) | ( cd "$root" && tar -xf - )
  local script
  for script in postinst prerm postrm preinst; do
    if [ -f "$deb_dir/$script" ]; then cp "$deb_dir/$script" "$root/DEBIAN/$script"; chmod 0755 "$root/DEBIAN/$script"; fi
  done
  [ -f "$deb_dir/conffiles" ] && cp "$deb_dir/conffiles" "$root/DEBIAN/conffiles"
  local installed_size
  installed_size="$(( ( $(du -sk "$root" | awk '{print $1}') + 1023 ) / 1024 ))"
  sed -e "s|@VERSION@|$DEB_VERSION|" -e "s|@ARCH@|$ARCH|" \
      -e "s|@DEPENDS@|$depends|" -e "s|@INSTALLED_SIZE@|$installed_size|" \
      "$deb_dir/control.in" > "$root/DEBIAN/control"
  ( cd "$root" && find . -type f ! -path './DEBIAN/*' -printf '%P\n' | LC_ALL=C sort | xargs -r md5sum > DEBIAN/md5sums )
  find "$root" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} + 2>/dev/null || true
  rm -f "$out"
  dpkg-deb --root-owner-group -Zxz --build "$root" "$out" > /dev/null
  log "  deb: $(basename "$out") ($(size_of "$out") 字节)"
}

tar_reproducible() {  # $1=out $2=父目录 $3=顶层目录名
  local out="$1" parent="$2" top="$3"
  rm -f "$out"
  tar --format=gnu --sort=name --owner=0 --group=0 --numeric-owner \
      --mtime="@$SOURCE_DATE_EPOCH" -C "$parent" -cJf "$out" "$top"
  log "  tar: $(basename "$out") ($(size_of "$out") 字节)"
}

# ============================================================
# 客户端
# ============================================================
stage_client() {
  log "== staging 客户端（scripts/stage-client-release.sh，唯一的真值来源）=="
  MAKEFLAGS="-j$JOBS" bash scripts/stage-client-release.sh --out dist/client --version "$VERSION" \
    > "$WORK/stage-client.log" 2>&1 || { tail -30 "$WORK/stage-client.log" >&2; die "客户端 staging 失败"; }
  tail -6 "$WORK/stage-client.log"
}

build_appdir() {
  local appdir="$WORK/appdir"
  log "== 组装 AppDir（我们的二进制 + QML 资源 + 图标）=="
  rm -rf "$appdir"
  mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" \
           "$appdir/usr/share/backup-project" "$appdir/usr/share/doc/backup-project"
  install -m 0755 dist/client/bin/backupctl "$appdir/usr/bin/backupctl"
  install -m 0755 dist/client/bin/backup-gui-modern "$appdir/usr/bin/backup-gui-modern"
  install -m 0755 packaging/client/AppRun "$appdir/AppRun"
  install -m 0644 packaging/client/AppImage.desktop "$appdir/backup-project.desktop"
  install -m 0644 packaging/client/AppImage.desktop "$appdir/usr/share/applications/backup-project.desktop"
  install -m 0644 resources/security/official-root-ed25519.pub "$appdir/usr/share/backup-project/"
  local doc
  for doc in docs/client-quick-start.md docs/release-layout.md docs/secure_transport.md docs/install-client.md; do
    [ -f "$doc" ] && install -m 0644 "$doc" "$appdir/usr/share/doc/backup-project/"
  done
  write_build_info "$appdir/usr/share/backup-project" "client-appdir"
  printf '%s\n' "$VERSION" > "$appdir/usr/share/backup-project/VERSION"

  # 图标要在 linuxdeploy **之前**就位：linuxdeploy 会按 desktop 文件的 Icon=
  # 去 AppDir 里找图标文件，找不到就直接失败。图标本身来自仓库自己的
  # AppIcon.qml（不是新画的品牌，见 make-icon.py）。
  python3 packaging/client/make-icon.py "$appdir/backup-project.png" 256 > "$WORK/icon.log" 2>&1 || {
    tail -5 "$WORK/icon.log" >&2; die "生成图标失败（需要 python3-pil）"; }
  cat "$WORK/icon.log"
  install -d -m 0755 "$appdir/usr/share/icons/hicolor/256x256/apps"
  install -m 0644 "$appdir/backup-project.png" "$appdir/usr/share/icons/hicolor/256x256/apps/backup-project.png"

  if [ "$SKIP_APPIMAGE" -eq 1 ]; then
    log "--skip-appimage：只装我们的二进制，不打包 Qt 运行时"
    return 0
  fi

  bash packaging/fetch-tools.sh > "$WORK/fetch-tools.log" 2>&1 || { tail -20 "$WORK/fetch-tools.log" >&2; die "打包工具下载/校验失败"; }
  local tools="$PACKAGING_DIR/.tools"
  mkdir -p "$WORK/bin"
  install -m 0755 "$tools/linuxdeploy-x86_64.AppImage" "$WORK/bin/linuxdeploy"
  install -m 0755 "$tools/linuxdeploy-plugin-qt-x86_64.AppImage" "$WORK/bin/linuxdeploy-plugin-qt"

  # 我们的 QML 是编进二进制的（qrc），linuxdeploy-plugin-qt 扫不到，必须显式
  # 告诉它去哪读 import 依赖；否则会出现"能构建、装到干净机器上缺 QML 模块"。
  export QML_SOURCES_PATHS="$REPO_ROOT/ui/modern/qml"
  export EXTRA_QT_MODULES="QtQuick;QtQml;QtQuick.Controls;QtQuick.Layouts;QtQuick.Dialogs;QtQuick.Window;QtNetwork;QtConcurrent"
  # offscreen / minimal 平台插件也要带上：CI 与无显示环境（容器、远程维护）都要能
  # 启动同一个 AppImage，而不是只支持有 X 的机器。每个插件只有几十 KB。
  export EXTRA_QT_PLUGINS="platforms/libqoffscreen.so;platforms/libqminimal.so"
  export APPIMAGE_EXTRACT_AND_RUN=1
  export ARCH=x86_64
  export PATH="$WORK/bin:$PATH"

  log "== linuxdeploy + plugin-qt（带 Qt 运行时与 QML 模块）=="
  if ! "$WORK/bin/linuxdeploy" --appdir "$appdir" --plugin qt \
       --desktop-file "$appdir/usr/share/applications/backup-project.desktop" \
       --icon-file "$appdir/backup-project.png" --output appimage \
       > "$WORK/linuxdeploy.log" 2>&1; then
    tail -40 "$WORK/linuxdeploy.log" >&2
    die "linuxdeploy 失败"
  fi
  tail -12 "$WORK/linuxdeploy.log"
}

pack_client_appimage() {
  local appdir="$WORK/appdir"
  local tools="$PACKAGING_DIR/.tools"
  local out="$RELEASE_DIR/Backup-Project-Client-$VERSION-x86_64.AppImage"
  log "== appimagetool =="
  # AppDir 里的 BUILD-INFO/MANIFEST 必须反映最终内容：Qt 是 linuxdeploy 之后
  # 才进去的，所以清单在这里（打包前）重新生成。
  rm -f "$appdir/MANIFEST.sha256"
  ( cd "$appdir" && find . -type f ! -name MANIFEST.sha256 -printf '%P\n' | LC_ALL=C sort | xargs -r sha256sum > MANIFEST.sha256 )
  find "$appdir" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} + 2>/dev/null || true
  rm -f "$out"
  VERSION="$VERSION" ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    "$tools/appimagetool-x86_64.AppImage" --no-appstream "$appdir" "$out" > "$WORK/appimagetool.log" 2>&1 || {
      tail -30 "$WORK/appimagetool.log" >&2; die "appimagetool 失败"; }
  [ -f "$out" ] || die "appimagetool 没有产出 $out"
  log "  AppImage: $(basename "$out") ($(size_of "$out") 字节)"
}

pack_client_tarball() {
  local appdir="$WORK/appdir"
  local stage_root="$WORK/client-tar"
  local top="Backup-Project-Client-$VERSION-linux-x86_64"
  log "== 客户端 portable tar.xz =="
  rm -rf "$stage_root"; mkdir -p "$stage_root/$top"
  local tree="$stage_root/$top"
  local part
  for part in bin lib plugins qml; do
    [ -d "$appdir/usr/$part" ] && cp -a "$appdir/usr/$part" "$tree/"
  done
  mkdir -p "$tree/share/backup-project" "$tree/docs" "$tree/packaging/client" "$tree/packaging/portable"
  install -m 0644 resources/security/official-root-ed25519.pub "$tree/share/backup-project/"
  printf '%s\n' "$VERSION" > "$tree/share/backup-project/VERSION"
  local doc
  for doc in docs/client-quick-start.md docs/release-layout.md docs/secure_transport.md docs/install-client.md docs/release-packaging.md; do
    [ -f "$doc" ] && install -m 0644 "$doc" "$tree/docs/"
  done
  install -m 0755 packaging/portable/install-client.sh "$tree/install.sh"
  install -m 0755 packaging/portable/uninstall-client.sh "$tree/uninstall.sh"
  install -m 0644 packaging/client/backup-project.desktop "$tree/packaging/client/"
  install -m 0755 packaging/portable/uninstall-client.sh "$tree/packaging/portable/"
  write_build_info "$tree" client
  write_manifest "$tree" > /dev/null
  tar_reproducible "$RELEASE_DIR/backup-project-client-$VERSION-linux-x86_64.tar.xz" "$stage_root" "$top"
}

pack_client_deb() {
  local stage_root="$WORK/client-deb"
  local tree="$stage_root"
  log "== 客户端 .deb =="
  rm -rf "$stage_root"
  mkdir -p "$tree/usr/lib/backup-project-client" "$tree/usr/bin" \
           "$tree/usr/share/applications" "$tree/usr/share/doc/backup-project-client"
  local part
  for part in bin lib plugins qml; do
    [ -d "$WORK/appdir/usr/$part" ] && cp -a "$WORK/appdir/usr/$part" "$tree/usr/lib/backup-project-client/"
  done
  install -m 0755 packaging/client/wrappers/backupctl.sh "$tree/usr/bin/backupctl"
  install -m 0755 packaging/client/wrappers/backup-project "$tree/usr/bin/backup-project"
  install -m 0644 packaging/client/backup-project.desktop "$tree/usr/share/applications/backup-project.desktop"
  install -d -m 0755 "$tree/usr/lib/backup-project-client/share/backup-project"
  install -m 0644 resources/security/official-root-ed25519.pub "$tree/usr/lib/backup-project-client/share/backup-project/"
  printf '%s\n' "$VERSION" > "$tree/usr/lib/backup-project-client/share/backup-project/VERSION"
  local doc
  for doc in docs/client-quick-start.md docs/release-layout.md docs/secure_transport.md docs/install-client.md docs/release-packaging.md; do
    [ -f "$doc" ] && install -m 0644 "$doc" "$tree/usr/share/doc/backup-project-client/"
  done
  write_build_info "$tree/usr/share/doc/backup-project-client" client
  write_manifest "$tree/usr/share/doc/backup-project-client" > /dev/null
  cat > "$WORK/third-party-notices-client.txt" <<EOF
本发行包动态链接 Qt（版本 $QT_VERSION，来自构建基线的发行版软件包，LGPL-3.0）。
Qt 以动态库形式随包提供，位于 /usr/lib/backup-project-client/lib/ 与
/usr/lib/backup-project-client/qml/，可以被替换（也可被移除，改用系统 Qt）。
Qt 源码： https://download.qt.io/official_releases/qt/
打包工具（构建期使用，不随包分发）：见 packaging/tools.lock —— linuxdeploy、
linuxdeploy-plugin-qt、appimagetool，各自的版本、来源 URL、sha256 与许可证。
EOF
  install -m 0644 "$WORK/third-party-notices-client.txt" "$tree/usr/share/doc/backup-project-client/THIRD-PARTY-NOTICES.txt"
  cat > "$tree/usr/share/doc/backup-project-client/copyright" <<EOF
backup-project-client $VERSION

上游仓库：https://github.com/PlyusWang/backup-project
上游仓库（commit $COMMIT）**没有 LICENSE 文件**，因此本包不对上游代码的许可
做任何声明或推断 —— 这一点如实记在这里，而不是替权利人补一个许可证。
随包分发的第三方组件：Qt $QT_VERSION（LGPL-3.0，动态链接，见 THIRD-PARTY-NOTICES.txt）。
EOF
  gzip -9n -c "$tree/usr/share/doc/backup-project-client/copyright" > /dev/null 2>&1 || true
  # 依赖来自两处：CLI 自己链接的系统库，以及**随包的 Qt 库**各自链接的系统库
  # （X11 / xcb / GL / fontconfig 这些不随包，必须由目标机器的发行版提供）。
  # 不把 GUI 二进制直接交给 dpkg-shlibdeps：它链接的 Qt 是我们自己带的，
  # dpkg 里查不到对应的包来源，会误报 "no dependency information found"。
  local depends qt_libs
  qt_libs="$(find "$tree/usr/lib/backup-project-client/lib" -name 'libQt6*.so.6*' -type f 2>/dev/null | LC_ALL=C sort | tr '\n' ' ')"
  # shellcheck disable=SC2086
  depends="$(shlibs_depends "$tree/usr/bin/backupctl" $qt_libs 2>/dev/null || true)"
  [ -n "$depends" ] || die "dpkg-shlibdeps 没有算出依赖（拒绝手写依赖列表）"
  # 我们**自带** Qt，所以目标机不该被要求装发行版的 Qt 包：把 dpkg-shlibdeps
  # 从"随包的 Qt 库"里推出来的 libqt6* / qt6-base-abi 去掉，只留下目标机必须
  # 提供的系统库（libc / libstdc++ / X11 / xcb / GL / fontconfig 等）。
  local filtered="" entry
  local IFS=','
  for entry in $depends; do
    case "$entry" in
      *libqt6*|*qt6-base-abi*) continue ;;
    esac
    filtered="${filtered:+$filtered, }$(printf '%s' "$entry" | sed -e 's/^ *//' -e 's/ *$//')"
  done
  unset IFS
  depends="$filtered"
  [ -n "$depends" ] || die "过滤掉自带的 Qt 之后依赖为空，说明推算出错了"
  case "$depends" in *libqt6*|*qt6-base-abi*) die "依赖里仍残留发行版 Qt 包" ;; esac
  log "  依赖（dpkg-shlibdeps，已去掉自带的 Qt）：$depends"
  build_deb packaging/client/deb "$tree" "$RELEASE_DIR/backup-project-client_${DEB_VERSION}_${ARCH}.deb" "$depends"
}

# ============================================================
# 服务端
# ============================================================
stage_server() {
  log "== staging 服务端（scripts/stage-server-release.sh）=="
  MAKEFLAGS="-j$JOBS" bash scripts/stage-server-release.sh --out dist/server --version "$VERSION" \
    > "$WORK/stage-server.log" 2>&1 || { tail -30 "$WORK/stage-server.log" >&2; die "服务端 staging 失败"; }
  tail -6 "$WORK/stage-server.log"
  [ -f dist/server/bin/backup-server-admin.sh ] || die "服务端 staging 里没有管理员菜单脚本（scripts/backup-server-admin.sh）"
}

server_payload() {  # $1 = 目标树根
  local tree="$1"
  local bin="$tree/usr/lib/backup-project-server/bin"
  mkdir -p "$bin" "$tree/usr/lib/backup-project-server/share/backup-project" \
           "$tree/usr/lib/backup-project-server/share/doc/backup-project" \
           "$tree/usr/bin" "$tree/usr/share/doc/backup-project-server"
  local tool
  for tool in backup-server backup-server-admin backup-server-keygen backup-cert-tool; do
    install -m 0755 "dist/server/bin/$tool" "$bin/$tool" 2>/dev/null || install -m 0755 "dist/server/tools/$tool" "$bin/$tool"
  done
  install -m 0755 dist/server/bin/backup-server-admin.sh "$bin/backup-server-admin.sh"
  install -m 0755 packaging/server/launch-server.sh "$bin/launch-server.sh"
  install -m 0755 packaging/server/purge-data.sh "$bin/purge-data.sh"
  install -m 0644 resources/security/official-root-ed25519.pub "$tree/usr/lib/backup-project-server/share/backup-project/"
  printf '%s\n' "$VERSION" > "$tree/usr/lib/backup-project-server/share/backup-project/VERSION"
  local doc
  for doc in docs/server-quick-start.md docs/self-hosted-server.md docs/release-layout.md docs/secure_transport.md \
             docs/install-server.md docs/upgrade-server.md docs/release-packaging.md; do
    [ -f "$doc" ] && install -m 0644 "$doc" "$tree/usr/lib/backup-project-server/share/doc/backup-project/"
  done
  local wrapper
  for wrapper in backup-server backup-project-server backup-server-admin backup-server-admin-menu \
                 backup-server-keygen backup-cert-tool backup-server-purge-data; do
    case "$wrapper" in
      backup-project-server) install -m 0755 packaging/server/wrappers/backup-server "$tree/usr/bin/$wrapper" ;;
      *) install -m 0755 "packaging/server/wrappers/$wrapper" "$tree/usr/bin/$wrapper" ;;
    esac
  done
  install -d -m 0755 "$tree/etc/backup-project-server"
  install -m 0644 packaging/server/server.conf "$tree/etc/backup-project-server/server.conf"
  install -d -m 0755 "$tree/lib/systemd/system"
  install -m 0644 packaging/server/backup-project-server.service "$tree/lib/systemd/system/backup-project-server.service"
  install -d -m 0755 "$tree/usr/lib/tmpfiles.d"
  install -m 0644 packaging/server/tmpfiles.conf "$tree/usr/lib/tmpfiles.d/backup-project-server.conf"
}

pack_server_deb() {
  local tree="$WORK/server-deb"
  log "== 服务端 .deb =="
  rm -rf "$tree"; mkdir -p "$tree"
  server_payload "$tree"
  write_build_info "$tree/usr/share/doc/backup-project-server" server
  write_manifest "$tree/usr/share/doc/backup-project-server" > /dev/null
  cat > "$tree/usr/share/doc/backup-project-server/copyright" <<EOF
backup-project-server $VERSION

上游仓库：https://github.com/PlyusWang/backup-project
上游仓库（commit $COMMIT）**没有 LICENSE 文件**，本包不对上游代码的许可做任何
声明或推断。本包不包含任何私钥：传输身份私钥由 backup-server-keygen 在目标机
本机生成（0600），根私钥从不离开离线机器。
EOF
  cp packaging/server/deb/conffiles "$tree/DEBIAN-conffiles" 2>/dev/null || true
  local tmp_deb_dir="$WORK/server-deb-meta"
  rm -rf "$tmp_deb_dir"; mkdir -p "$tmp_deb_dir"
  cp packaging/server/deb/control.in packaging/server/deb/postinst packaging/server/deb/prerm packaging/server/deb/postrm "$tmp_deb_dir/"
  printf '/etc/backup-project-server/server.conf\n' > "$tmp_deb_dir/conffiles"
  local depends
  depends="$(shlibs_depends "$tree/usr/lib/backup-project-server/bin/backup-server" \
              "$tree/usr/lib/backup-project-server/bin/backup-server-admin" \
              "$tree/usr/lib/backup-project-server/bin/backup-server-keygen" \
              "$tree/usr/lib/backup-project-server/bin/backup-cert-tool" 2>/dev/null || true)"
  [ -n "$depends" ] || die "dpkg-shlibdeps 没有算出服务端依赖"
  # adduser 是 Essential:yes，但显式写上更清楚；systemd 只在真机安装时需要。
  depends="$depends, adduser, init-system-helpers (>= 1.51)"
  log "  依赖（dpkg-shlibdeps）：$depends"
  build_deb "$tmp_deb_dir" "$tree" "$RELEASE_DIR/backup-project-server_${DEB_VERSION}_${ARCH}.deb" "$depends"
}

pack_server_tarball() {
  local stage_root="$WORK/server-tar"
  local top="backup-project-server-$VERSION-linux-x86_64"
  log "== 服务端 portable tar.xz =="
  rm -rf "$stage_root"; mkdir -p "$stage_root/$top"
  local tree="$stage_root/$top"
  mkdir -p "$tree/share" "$tree/docs" "$tree/packaging/server" "$tree/packaging/portable"
  server_payload "$tree/portable-root"
  # deb 布局 -> portable 布局：/usr/lib/backup-project-server/bin -> bin/
  rm -rf "$tree/bin" "$tree/etc"
  mv "$tree/portable-root/usr/lib/backup-project-server/bin" "$tree/bin"
  mv "$tree/portable-root/usr/lib/backup-project-server/share/backup-project" "$tree/share/backup-project"
  mv "$tree/portable-root/usr/lib/backup-project-server/share/doc/backup-project/." "$tree/docs/" 2>/dev/null || true
  install -m 0755 packaging/portable/install-server.sh "$tree/install.sh"
  install -m 0755 packaging/portable/uninstall.sh "$tree/uninstall.sh"
  install -m 0644 packaging/server/server.conf "$tree/packaging/server/server.conf"
  install -m 0644 packaging/server/backup-project-server.service "$tree/packaging/server/"
  install -m 0755 packaging/portable/uninstall.sh "$tree/packaging/portable/"
  install -m 0755 packaging/portable/install-server.sh "$tree/packaging/portable/"
  rm -rf "$tree/portable-root"
  write_build_info "$tree" server
  write_manifest "$tree" > /dev/null
  tar_reproducible "$RELEASE_DIR/backup-project-server-$VERSION-linux-x86_64.tar.xz" "$stage_root" "$top"
}

# ============================================================
# 顶层流程
# ============================================================
if [ "$ONLY" = "all" ] || [ "$ONLY" = "client" ]; then
  if [ "$SKIP_STAGE" -eq 0 ]; then stage_client; fi
  build_appdir
  pack_client_tarball
  pack_client_deb
  if [ "$SKIP_APPIMAGE" -eq 0 ]; then pack_client_appimage; fi
fi

if [ "$ONLY" = "all" ] || [ "$ONLY" = "server" ]; then
  if [ "$SKIP_STAGE" -eq 0 ]; then stage_server; fi
  pack_server_deb
  pack_server_tarball
fi

# ---- release 元数据 ----
log "== SHA256SUMS 与 RELEASE-INFO.txt =="
( cd "$RELEASE_DIR" && find . -maxdepth 1 -type f ! -name SHA256SUMS ! -name RELEASE-INFO.txt -printf '%P\n' | LC_ALL=C sort | xargs -r sha256sum > SHA256SUMS )
{
  echo "product       = Backup Project"
  echo "version       = $VERSION"
  echo "deb_version   = $DEB_VERSION"
  echo "commit        = $COMMIT"
  echo "tree          = $TREE"
  echo "branch        = $BRANCH"
  echo "architecture  = $ARCH (linux x86_64)"
  echo "build_os      = $OS_PRETTY"
  echo "build_glibc   = $GLIBC_VERSION"
  echo "compiler      = $CXX_VERSION"
  echo "qt_version    = $QT_VERSION"
  echo "source_date_epoch = $SOURCE_DATE_EPOCH"
  echo "build_timestamp   = $STAMP"
  echo "working_tree  = clean"
  echo
  echo "客户端： AppImage（免安装，自带 Qt）/ .deb / portable tar.xz"
  echo "服务端： .deb（systemd + 管理菜单 + 专用系统用户）/ portable tar.xz"
  echo "本目录不含私钥、token secret、证书私钥材料。"
} > "$RELEASE_DIR/RELEASE-INFO.txt"

# ---- 制品自查：私钥扫描 + 清单核对 ----
log "== 制品私钥扫描 =="
SCAN_TARGET="$WORK/scan"
rm -rf "$SCAN_TARGET"; mkdir -p "$SCAN_TARGET"
for artifact in "$RELEASE_DIR"/*; do
  case "$artifact" in *SHA256SUMS|*RELEASE-INFO.txt) continue ;; esac
  base="$(basename "$artifact")"
  case "$artifact" in
    *.deb) dpkg-deb -x "$artifact" "$SCAN_TARGET/$base" ;;
    *.tar.xz) mkdir -p "$SCAN_TARGET/$base" && tar -xf "$artifact" -C "$SCAN_TARGET/$base" ;;
    *.AppImage) mkdir -p "$SCAN_TARGET/$base" && ( cd "$SCAN_TARGET/$base" && "$artifact" --appimage-extract >/dev/null 2>&1 || true ) ;;
  esac
done
hits="$(scan_for_secrets "$SCAN_TARGET")"
if [ "$hits" != "0" ]; then
  die "制品里发现了疑似私钥/口令内容（$hits 处）"
fi
log "  私钥扫描：0 命中"
if [ -n "$(find "$SCAN_TARGET" \( -name '*.key' -o -name 'secrets.env' \) -print -quit)" ]; then
  die "制品里出现了 .key / secrets.env 文件"
fi
log "  文件级检查：无 .key / secrets.env"

log "== 完成 =="
( cd "$RELEASE_DIR" && ls -l && echo && cat SHA256SUMS )
# third_party/sqlite

本目录只有一样东西：SQLite 的官方公共头文件 sqlite3.h。

## 为什么在这里

PR #20 的 backup-server 需要 SQLite 保存远程用户与快照元数据。
Ubuntu 的 libsqlite3-0（运行时库）是系统预装的，但 libsqlite3-dev
（头文件 + 开发符号链接）不一定在——本项目的开发虚拟机上就没有，
而夜班开发不允许使用 sudo 安装系统包。

所以这里固定一份**官方头文件**，按上游发布内容原样保存。链接时仍然使用
系统自带的 libsqlite3.so.0，没有把 SQLite 源码或二进制带进仓库。

## 来源

    package   libsqlite3-dev
    version   3.45.1-1ubuntu2.8
    arch      amd64
    file      libsqlite3-dev_3.45.1-1ubuntu2.8_amd64.deb
    sha256    61e688e7ced1c6f7c0b66721e46fd996f891489654dbf382daa98e17ee3ae192
    member    /usr/include/sqlite3.h
    obtained  apt-get download libsqlite3-dev   （不需要 root）

校验：把上面那个 .deb 展开后取出的 usr/include/sqlite3.h 与本目录文件
逐字节相同，sha256 见同目录的 sqlite3.h.sha256。

## 许可

SQLite 是 public domain（见 sqlite3.h 文件头的声明）。随仓库分发头文件
没有许可问题。

## 构建期怎么用

Makefile 的发现顺序：

1. /usr/include/sqlite3.h（装了 libsqlite3-dev 的机器）；
2. 本目录的 include/sqlite3.h。

链接一律直接指向系统运行库（libsqlite3.so.0），不依赖 -lsqlite3 这条
开发符号链接，因此在只有运行时库的机器上也能链接。

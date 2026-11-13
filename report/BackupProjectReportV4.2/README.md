# Backup Project 第 5 组实验报告 V4.2

本报告对应软件开发综合实验第 5 组。报告 PDF：`output/软件开发综合实验_小组实验报告.pdf`。

## 重新编译

安装 TeX Live，使用 XeLaTeX。字体由 TeX Live 提供。

Windows：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\build-windows.ps1
```

Linux：

```bash
bash build-linux.sh
```

编译脚本执行两遍 XeLaTeX，将结果保存到上述 PDF，自动清理编译中间文件。

## 文件

- `main_Linux.tex`、`main_Windows.tex`：编译入口。
- `style.tex`、`report-content.tex`：版式与正文组织。
- `fonts/`：两个系统入口的字体配置。
- `sections/`、`tables/`：正文与正文引用的表格。
- `figures/`：正文实际使用的图纸和测试截图。
- `gantt/`：正文引用的甘特图 TeX 文件。

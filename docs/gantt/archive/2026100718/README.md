# Gantt 快照：2026-10-07 18:1x（更新前）

本目录存的是 **更新前** 的那一版甘特图，也就是：

- `project_gantt.tex`：最后一次随 `docs: update project gantt for sprint cadence`
  （2026-09-10，提交 6fbba47）入库的版本，脚注中的“当前节点”还停留在第 2 次课；
- `project_gantt.pdf`：由上面这份 .tex 在 2026-09-10 编译出来的 PDF，两轮之后
  一直没再编译过。

## 为什么只有 .tex 被更新，PDF 没跟着更新

更新 Gantt 的这台机器上**没有安装 LaTeX**（`pdflatex` / `xelatex` /
`latexmk` 都不存在），所以无法重新编译。`docs/gantt/project_gantt.tex` 已经
更新到 Course Closeout 阶段，而 `docs/gantt/project_gantt.pdf` 仍是本目录
这一份 2026-09-10 的 PDF —— 这两者目前**不对应**，需要在有 LaTeX 的机器上执行：

```bash
xelatex project_gantt.tex        # ctexart 建议用 xelatex
```

重新生成后再替换 `docs/gantt/project_gantt.pdf`。没有伪造一份新 PDF。

#!/usr/bin/env bash
set -Eeuo pipefail
cd "$(dirname "$0")"
mkdir -p output
build_temp="$(mktemp -d -t BackupProjectReport.XXXXXXXX)"
trap 'rm -rf -- "$build_temp"' EXIT
for pass in 1 2; do
  xelatex -interaction=nonstopmode -halt-on-error -file-line-error \
    -output-directory="$build_temp" -jobname=report main_Linux.tex
done
cp "$build_temp/report.pdf" "output/软件开发综合实验_小组实验报告.pdf"
echo '完成：output/软件开发综合实验_小组实验报告.pdf'

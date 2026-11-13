$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
New-Item -ItemType Directory -Force -Path output | Out-Null
$buildTemp = Join-Path ([IO.Path]::GetTempPath()) ('BackupProjectReport-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $buildTemp | Out-Null
try {
    foreach ($pass in 1..2) {
        & xelatex -interaction=nonstopmode -halt-on-error -file-line-error "-output-directory=$buildTemp" -jobname=report main_Windows.tex
        if ($LASTEXITCODE -ne 0) { throw "XeLaTeX 第 $pass 次编译失败" }
    }
    Copy-Item -LiteralPath (Join-Path $buildTemp 'report.pdf') -Destination 'output/软件开发综合实验_小组实验报告.pdf' -Force
}
finally {
    $resolvedTemp = (Resolve-Path -LiteralPath $buildTemp).Path
    if ($resolvedTemp -ne [IO.Path]::GetFullPath($buildTemp) -or (Split-Path -Leaf $resolvedTemp) -notlike 'BackupProjectReport-*') {
        throw '编译临时目录路径核验失败'
    }
    Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
}
Write-Output '完成：output/软件开发综合实验_小组实验报告.pdf'

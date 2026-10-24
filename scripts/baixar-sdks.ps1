# Baixa os SDKs de upscaling usados pelo port (DLSS, FidelityFX e XeSS) das fontes oficiais, nas versões testadas,
# para third_party/. Eles não ficam no repositório: cada um tem a licença do seu fabricante.
# Uso: powershell -ExecutionPolicy Bypass -File scripts\baixar-sdks.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root 'third_party'
$tmp = Join-Path $env:TEMP ('sdks-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null

function Sparse($url, $tag, $dir, $paths) {
    # git writes warnings to stderr, which Windows PowerShell would take for errors: its exit code decides
    $ErrorActionPreference = 'Continue'
    git clone --quiet --depth 1 --branch $tag --filter=blob:none --sparse $url $dir 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git clone $url ($tag) falhou" }
    git -C $dir sparse-checkout set --no-cone @paths 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git sparse-checkout em $dir falhou" }
}

try {
    # NVIDIA DLSS v310.9.1
    $d = Join-Path $tmp 'dlss'
    Sparse 'https://github.com/NVIDIA/DLSS.git' 'v310.9.1' $d @('/include/', '/lib/Windows_x86_64/rel/nvngx_dlss.dll', '/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib', '/LICENSE.txt')
    $o = Join-Path $tp 'dlss'
    New-Item -ItemType Directory -Force "$o\bin", "$o\lib", "$o\include" | Out-Null
    Copy-Item "$d\include\*" "$o\include\" -Recurse -Force
    Copy-Item "$d\lib\Windows_x86_64\rel\nvngx_dlss.dll" "$o\bin\" -Force
    Copy-Item "$d\lib\Windows_x86_64\x64\nvsdk_ngx_d.lib" "$o\lib\" -Force
    Copy-Item "$d\LICENSE.txt" "$o\" -Force

    # AMD FidelityFX SDK v2.3.0 (FSR)
    $f = Join-Path $tmp 'ffx'
    Sparse 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git' 'v2.3.0' $f @('/Kits/FidelityFX/api/include/', '/Kits/FidelityFX/upscalers/include/', '/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll', '/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll', '/readme.md')
    $o = Join-Path $tp 'ffx'
    New-Item -ItemType Directory -Force "$o\api", "$o\upscalers", "$o\bin" | Out-Null
    Copy-Item "$f\Kits\FidelityFX\api\include" "$o\api\" -Recurse -Force
    Copy-Item "$f\Kits\FidelityFX\upscalers\include" "$o\upscalers\" -Recurse -Force
    Copy-Item "$f\Kits\FidelityFX\signedbin\amd_fidelityfx_*_dx12.dll" "$o\bin\" -Force
    Copy-Item "$f\readme.md" "$o\readme.md" -Force

    # Intel XeSS 3.0.2
    $zip = Join-Path $tmp 'xess.zip'
    Invoke-WebRequest 'https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip' -OutFile $zip -UseBasicParsing
    $x = Join-Path $tmp 'xess'
    New-Item -ItemType Directory -Force $x | Out-Null
    # Windows' own tar (libarchive) reads zip; another tar first in the PATH may not
    & "$env:SystemRoot\System32\tar.exe" -xf $zip -C $x
    # the package may sit in a folder of its own: wherever inc\xess is
    $x = (Get-ChildItem $x -Recurse -Filter 'xess.h' | Where-Object { $_.Directory.Name -eq 'xess' } | Select-Object -First 1).Directory.Parent.Parent.FullName
    $o = Join-Path $tp 'xess'
    New-Item -ItemType Directory -Force "$o\bin", "$o\include" | Out-Null
    Copy-Item "$x\inc\xess\*" "$o\include\" -Force
    Copy-Item "$x\bin\libxess.dll" "$o\bin\" -Force
    Copy-Item "$x\LICENSE.txt" "$o\" -Force
    'SDKs prontos em ' + $tp
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

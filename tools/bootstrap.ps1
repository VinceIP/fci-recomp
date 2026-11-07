# BuildGame.cmd and BuildEditor.cmd: a private Python for fci-recomp.py, then fci-recomp.py itself.
#
#   bootstrap.ps1 setup <disc> [more fci-recomp.py options]
#   bootstrap.ps1 editor [options]
#
# The release carries Python with capstone in python\ (tools/package.py), so nothing is downloaded or
# installed. A git checkout has none: there Python comes from the Python Software Foundation's NuGet
# package (a plain copy of Python, no installer) and capstone from PyPI, both checked against the
# digests in data\python.json, and is kept in build\python.
param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $Rest)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is many times slower with its progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Repo = Split-Path -Parent $PSScriptRoot
$Bundled = Join-Path $Repo 'python'
if (Test-Path $Bundled) {
    $BundledPython = Join-Path $Bundled 'python.exe'
    if (-not (Test-Path $BundledPython)) {
        Write-Host 'error: the bundled Python (python\python.exe) is missing. Extract the whole release zip again;'
        Write-Host 'an antivirus may have removed some of its files.'
        exit 1
    }
    & $BundledPython -B (Join-Path $Repo 'fci-recomp.py') @Rest
    exit $LASTEXITCODE
}
$Root = Join-Path $Repo 'build\python'
$Python = Join-Path $Root 'tools\python.exe'

$Pins = Get-Content -Raw (Join-Path $Repo 'data\python.json') | ConvertFrom-Json
$PythonPackage = @{ Url = $Pins.python.url; Sha512 = $Pins.python.sha512 }
$Capstone = @{ Url = $Pins.capstone.url; Sha256 = $Pins.capstone.sha256 }
$Ready = "python $($Pins.python.version), capstone $($Pins.capstone.version)"

function Get-Checked($Url, $File, $Algorithm, $Expected) {
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $File
    $hex = (Get-FileHash -Algorithm $Algorithm $File).Hash
    if ($Algorithm -eq 'SHA512') {
        $bytes = [byte[]] (0..($hex.Length / 2 - 1) | ForEach-Object { [Convert]::ToByte($hex.Substring($_ * 2, 2), 16) })
        $actual = [Convert]::ToBase64String($bytes)
    } else {
        $actual = $hex.ToLower()
    }
    if ($actual -ne $Expected) {
        Remove-Item $File
        throw "$Url does not match its published digest; nothing was installed"
    }
}

if ($PSVersionTable.PSVersion.Major -lt 5) {
    Write-Host 'error: PowerShell 5 or newer is required.'
    exit 1
}

# an antivirus can remove the private Python after it was made: then it is made again
# made before, by this script or the launcher, from the same pins
$ReadyFile = Join-Path $Root 'ready'
$Made = if (Test-Path $ReadyFile) { (Get-Content -Raw $ReadyFile).Trim() } else { '' }
if ($Made -ne $Ready -or -not (Test-Path $Python)) {
    Write-Host 'Downloading Python...'
    try {
        if (Test-Path $Root) { Remove-Item -Recurse -Force $Root }
        New-Item -ItemType Directory -Force $Root | Out-Null
        $zip = Join-Path $Root 'python.zip'
        Get-Checked $PythonPackage.Url $zip 'SHA512' $PythonPackage.Sha512
        Expand-Archive $zip $Root
        Remove-Item $zip
        $wheel = Join-Path $Root 'capstone.zip'
        Get-Checked $Capstone.Url $wheel 'SHA256' $Capstone.Sha256
        Expand-Archive $wheel (Join-Path $Root 'tools\Lib\site-packages')
        Remove-Item $wheel
        & $Python -c 'import capstone, ssl, ctypes, winreg'
        if ($LASTEXITCODE) { throw 'Python couldn''t start' }
    } catch {
        Write-Host "error: Python failed to download. $($_.Exception.Message)"
        Write-Host 'Check the internet connection; a proxy, firewall or antivirus can also block the download.'
        exit 1
    }
    Set-Content $ReadyFile $Ready
}

& $Python -B (Join-Path $Repo 'fci-recomp.py') @Rest
exit $LASTEXITCODE

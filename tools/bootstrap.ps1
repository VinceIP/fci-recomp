# BuildGame.cmd and BuildEditor.cmd: a private Python for fci-recomp.py, then fci-recomp.py itself.
#
#   bootstrap.ps1 setup <disc> [more fci-recomp.py options]
#   bootstrap.ps1 editor [options]
#
# Python comes from the Python Software Foundation's NuGet package (a plain copy of Python, no
# installer) and capstone from PyPI; both are checked against the digests below and kept in
# build\python, so nothing is installed on the system and the next run starts at once.
param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $Rest)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is many times slower with its progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Repo = Split-Path -Parent $PSScriptRoot
$Root = Join-Path $Repo 'build\python'
$Python = Join-Path $Root 'tools\python.exe'

$PythonPackage = @{
    Url    = 'https://api.nuget.org/v3-flatcontainer/python/3.13.16/python.3.13.16.nupkg'
    Sha512 = 'TGEhuRTpWtRifDR4r/GEhmg/cGMqtwB4kOCkt1QhXnCmdDE3Dict3iLJI1xrsNSRbXGyCwLEhe6qa5Rd2s376w=='  # base64, as NuGet lists it
}
$Capstone = @{
    Url    = 'https://files.pythonhosted.org/packages/70/39/2138d890a8e827636b9de9924fbc8527fe83e38b6d26605b30ac55e30ebe/capstone-5.0.7-py3-none-win_amd64.whl'
    Sha256 = '4ab8bcb7da8f221ff45926ca168ca33e76f7237d06fbf3c10780002faa2670e1'
}

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
if (-not (Test-Path (Join-Path $Root 'ready')) -or -not (Test-Path $Python)) {
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
        Write-Host 'Check Internet connection.'
        exit 1
    }
    Set-Content (Join-Path $Root 'ready') 'python 3.13.16, capstone 5.0.7'
}

& $Python -B (Join-Path $Repo 'fci-recomp.py') @Rest
exit $LASTEXITCODE

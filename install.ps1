# install.ps1 - mongoosepro-arm64 driver + vendor J2534 stack installer.
# Extract the release zip and run from an elevated PowerShell prompt:
#   powershell -ExecutionPolicy Bypass -File .\install.ps1
# If J2534_Driver_x64.msi isn't next to the script it is downloaded from
# $VendorMsiUrl and must match the tested SHA256.
param(
  [string]$VendorMsi = (Join-Path $PSScriptRoot 'J2534_Driver_x64.msi'),
  [string]$VendorMsiUrl = 'https://d5t5.com/sw/J2534_Driver_x64.msi',
  [string]$DriverDir = $PSScriptRoot,
  [string]$OutputMsi = (Join-Path $PSScriptRoot 'J2534_Driver_arm64_nodrv.msi'),
  [string]$LogDir = (Join-Path $PSScriptRoot 'logs'),
  [switch]$NoInstall   # build the modified MSI only, don't run it
)
$ErrorActionPreference = 'Stop'
$rebootNeeded = $false

# ------------------------- helpers -------------------------
function Assert-Admin {
  $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
  if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an ELEVATED PowerShell prompt (Run as administrator).'
  }
}

function Assert-Arm64 {
  $k = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
  $arch = (Get-ItemProperty $k -EA SilentlyContinue).PROCESSOR_ARCHITECTURE
  if ($arch -ne 'ARM64') { throw "This installer is for Windows on ARM64 only (host arch: $arch)." }
}

function Assert-DriverFiles {
  foreach ($f in 'dtmonpro.sys','dtmonpro.inf','dtmonpro.cat','mongoosepro-arm64-test.cer') {
    if (-not (Test-Path (Join-Path $DriverDir $f))) { throw "missing driver file in $DriverDir : $f" }
  }
}

function New-NoDrvMsi([string]$Src,[string]$Dst) {
  # Copy the vendor MSI and strip the DIFx driver-install actions, so only
  # the J2534 DLL + registry get installed (the x64 driver can't run here).
  Copy-Item $Src $Dst -Force
  $names = @('MsiProcessDrivers','MsiInstallDrivers','MsiUninstallDrivers','MsiRollbackInstall','MsiCleanupOnSuccess')
  $removed = 0
  $msi = New-Object -ComObject WindowsInstaller.Installer
  try {
    $db = $msi.OpenDatabase($Dst, 1)
    foreach ($a in $names) {
      foreach ($tbl in 'InstallExecuteSequence','InstallUISequence') {
        $v = $db.OpenView("DELETE FROM $tbl WHERE ``Action``='$a'")
        $v.Execute(); $v.Close()
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($v)
        $removed++
      }
    }
    $db.Commit()
  } finally {
    if ($db) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($db) }
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($msi)
    $db = $null; $msi = $null
    [GC]::Collect(); [GC]::WaitForPendingFinalizers()
  }
  # verify: reopen read-only, confirm no action rows survive, read ProductCode
  $wi = New-Object -ComObject WindowsInstaller.Installer
  try {
    $dbo = $wi.OpenDatabase($Dst, 0)
    $left = 0
    foreach ($a in $names) {
      foreach ($tbl in 'InstallExecuteSequence','InstallUISequence') {
        $v = $dbo.OpenView("SELECT ``Action`` FROM $tbl WHERE ``Action``='$a'")
        $v.Execute(); while ($v.Fetch()) { $left++ }; $v.Close()
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($v)
      }
    }
    if ($left -gt 0) { throw "nodrv MSI still contains $left driver-install action(s)" }
    $v = $dbo.OpenView("SELECT ``Value`` FROM Property WHERE ``Property``='ProductCode'")
    $v.Execute(); $r = $v.Fetch(); $code = $r.StringData(1); $v.Close()
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($v); [void][Runtime.InteropServices.Marshal]::ReleaseComObject($r)
    $global:ProductCode = $code
  } finally {
    if ($dbo) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($dbo) }
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($wi)
    $dbo = $null; $wi = $null
    [GC]::Collect(); [GC]::WaitForPendingFinalizers()
  }
  return $removed
}

# ------------------------- preflight -------------------------
New-Item $LogDir -ItemType Directory -Force | Out-Null
Start-Transcript (Join-Path $LogDir 'install.log') -Append | Out-Null
try {
  Assert-Admin
  Assert-Arm64
  Assert-DriverFiles
  $wantMsiHash = '4dc6d9f2c7f4379b8af4dcecf8a79ef3ba9d5fd190f7cd689c6cd0cac555f96f'
  $downloaded = $false
  if (-not (Test-Path $VendorMsi)) {
    "Vendor MSI not found locally; downloading $VendorMsiUrl"
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest $VendorMsiUrl -OutFile $VendorMsi -UseBasicParsing
    $downloaded = $true
  }
  $gotMsiHash = (Get-FileHash $VendorMsi -Algorithm SHA256).Hash.ToLower()
  if ($downloaded -and $gotMsiHash -ne $wantMsiHash) {
    Remove-Item $VendorMsi -Force
    throw "Downloaded vendor MSI SHA256 mismatch (got $gotMsiHash, want $wantMsiHash). Deleted it; supply the MSI manually with -VendorMsi."
  }
  if ($gotMsiHash -ne $wantMsiHash) {
    Write-Warning "Vendor MSI SHA256 does not match the version this was tested with.`n  got : $gotMsiHash`n  want: $wantMsiHash`nContinuing anyway."
  }

  # ------------------------- secure boot + test signing -------------------------
  try { $sb = Confirm-SecureBootUEFI } catch { $sb = $false }
  if ($sb) { throw 'Secure Boot is ON. Disable it in the VM firmware settings (VM > Settings > UEFI > Secure Boot), then rerun.' }
  $b = bcdedit /enum '{current}' | Out-String
  if ($b -notmatch 'testsigning\s+Yes') {
    bcdedit /set testsigning on | Out-Host
    $rebootNeeded = $true
    'testsigning enabled -- a reboot is required before the driver can load.'
  }

  # ------------------------- driver install -------------------------
  $cer = Join-Path $DriverDir 'mongoosepro-arm64-test.cer'
  $inf = Join-Path $DriverDir 'dtmonpro.inf'
  certutil -addstore -f Root $cer | Out-Host
  certutil -addstore -f TrustedPublisher $cer | Out-Host
  $p = pnputil /add-driver $inf /install 2>&1 | Out-String
  $code = $LASTEXITCODE; $p | Out-Host
  if ($code -eq 3010) { $rebootNeeded = $true }
  if (($code -notin 0,259,3010) -or ($p -notmatch 'added successfully|Driver package added')) {
    throw "pnputil failed: exit=$code`n$p"
  }
  $e = pnputil /enum-drivers | Out-String
  if ($e -notmatch 'dtmonpro\.inf' -or $e -notmatch 'mongoosepro-arm64 \(unofficial\)' -or $e -notmatch 'VehiclePassThru') {
    throw "driver not staged correctly:`n$e"
  }
  'native ARM64 driver staged OK'

  # ------------------------- modified vendor MSI -------------------------
  [void](New-NoDrvMsi -Src $VendorMsi -Dst $OutputMsi)
  "built $(Split-Path $OutputMsi -Leaf): DIFx driver-install actions stripped and verified (ProductCode=$global:ProductCode)"

  # ------------------------- vendor install -------------------------
  $installed = $false
  try {
    $wi2 = New-Object -ComObject WindowsInstaller.Installer
    try { $installed = ($wi2.ProductState($global:ProductCode) -eq 5) } catch { $installed = $false }
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($wi2)
  } catch { $installed = $false }

  if ($installed) {
    'vendor J2534 software already installed -- skipping MSI install.'
  } elseif (-not $NoInstall) {
    $mp = Start-Process msiexec -ArgumentList '/i',"`"$OutputMsi`"",'/qn','/norestart','/l*v',"`"$(Join-Path $LogDir 'vendor_msi.log')`"" -Wait -PassThru
    "msiexec exit=$($mp.ExitCode)"
    if ($mp.ExitCode -notin 0,3010) { throw "msiexec failed: $($mp.ExitCode) -- see $(Join-Path $LogDir 'vendor_msi.log')" }
    if ($mp.ExitCode -eq 3010) { $rebootNeeded = $true }
  }

  if (-not $NoInstall -or $installed) {
    $k = 'HKLM:\SOFTWARE\WOW6432Node\PassThruSupport.04.04\Drew Technologies Inc. - MongoosePro JLR'
    if (-not (Test-Path $k)) { throw "J2534 registry key missing: $k" }
    $fl = (Get-ItemProperty $k).FunctionLibrary
    "FunctionLibrary=$fl"
    if (-not (Test-Path $fl)) { throw "J2534 FunctionLibrary missing: $fl" }
    'vendor J2534 registration OK'
  }

  # ------------------------- summary -------------------------
  '==================== DONE ===================='
  'native ARM64 driver: staged (dtmonpro.inf, mongoosepro-arm64 unofficial)'
  if ($NoInstall) { "modified vendor MSI written to: $OutputMsi (not installed -- -NoInstall)" }
  elseif ($installed) { 'vendor J2534: already installed (skipped)' }
  else { 'vendor J2534: installed (monpj432.dll + PassThruSupport registry)' }
  if ($rebootNeeded) { Write-Warning 'REBOOT REQUIRED before plugging in the MongoosePro (testsigning and/or driver install queued a restart).' }
  $global:LASTEXITCODE = 0
  exit 0
} catch {
  Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
  exit 1
} finally { Stop-Transcript | Out-Null }

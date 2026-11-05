# cw_gen9.ps1 - the CodeWalker half of `stage_packs.mjs --gen9` (GTA V Enhanced car packs). Called by stage_packs; no window.
#
#   cw_gen9.ps1 -Mode extract -CodeWalker <dir> -In <dlc.rpf> -Out <dir> -GameDir <GTA V Legacy folder>
#       Decrypt-extract an NG/AES dlc.rpf, laid out like pack_dlc_rpf.mjs --extract (nested archives become folders named
#       *.rpf, resources keep their RSC7 header). The keys are read from the local game by CodeWalker and stay in this
#       process's memory: nothing here writes or prints a key.
#   cw_gen9.ps1 -Mode convert -CodeWalker <dir> -In <folder> -Out <folder>
#       Convert every .ytd .ydr .ydd .yft .ypt under <In> to the gen9 (Enhanced) resource format; other files are copied.
#       A headless replica of CodeWalker30_dev48 Tools > Asset Converter (ConvertAssetsForm, read from its IL):
#       RpfManager.IsGen9 = true; new XFile(); Load(bytes); Save(). Prints "<ext> <version>" per file.
#
# <CodeWalker> = the folder holding CodeWalker.Core.dll (CodeWalker30_dev48 or newer; the converter is not in older builds).
# Exit 1 on any failure.
param(
  [Parameter(Mandatory)][ValidateSet('extract', 'convert')][string]$Mode,
  [Parameter(Mandatory)][string]$CodeWalker,
  [Parameter(Mandatory)][string]$In,
  [Parameter(Mandatory)][string]$Out,
  [string]$GameDir
)
$ErrorActionPreference = 'Stop'
$core = Join-Path $CodeWalker 'CodeWalker.Core.dll'
if (-not (Test-Path $core)) { Write-Error "no CodeWalker.Core.dll in $CodeWalker"; exit 1 }
Set-Location $CodeWalker   # CodeWalker resolves its own data files relative to the working folder
[void][Reflection.Assembly]::LoadFrom($core)

if ($Mode -eq 'extract') {
  if (-not $GameDir) { Write-Error '-GameDir is required to extract'; exit 1 }
  [CodeWalker.GameFiles.GTA5Keys]::LoadFromPath($GameDir, $false, $null)
  $rf = New-Object CodeWalker.GameFiles.RpfFile($In, 'dlc.rpf')
  $errs = New-Object System.Collections.Generic.List[string]
  $rf.ScanStructure($null, [Action[string]]{ param($m) $errs.Add($m) })
  if ($errs.Count) { 'scan problems:'; $errs | Select-Object -First 5; exit 1 }
  $script:n = 0
  function Walk($file) {
    foreach ($e in $file.AllEntries) {
      if ($e -isnot [CodeWalker.GameFiles.RpfFileEntry]) { continue }
      $child = $file.FindChildArchive($e)
      if ($child) { Walk $child; continue }
      $data = $file.ExtractFile($e)
      if ($null -eq $data) { throw "extract returned nothing for $($e.Path)" }
      if ($e -is [CodeWalker.GameFiles.RpfResourceFileEntry] -and ($data.Length -lt 4 -or [BitConverter]::ToUInt32($data, 0) -ne 0x37435352)) {
        # ExtractFile hands a resource back DECOMPRESSED and headerless; a resource file on disk is the RSC7 header + the
        # deflated body (what the game, ytd_budget.mjs and the converter read).
        $data = [CodeWalker.GameFiles.ResourceBuilder]::AddResourceHeader($e, [CodeWalker.GameFiles.ResourceBuilder]::Compress($data))
      }
      $rel = $e.Path
      if ($rel.ToLowerInvariant().StartsWith('dlc.rpf\')) { $rel = $rel.Substring(8) }
      $dst = Join-Path $Out $rel
      New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
      [IO.File]::WriteAllBytes($dst, $data)
      $script:n++
    }
  }
  Walk $rf
  "extracted $($script:n) file(s)"
  if ($script:n -eq 0) { exit 1 }
  exit 0
}

$ok = 0; $copied = 0; $failed = 0
$was = [CodeWalker.GameFiles.RpfManager]::IsGen9
[CodeWalker.GameFiles.RpfManager]::IsGen9 = $true
try {
  $root = (Resolve-Path $In).Path.TrimEnd('\')
  foreach ($f in Get-ChildItem -Path $root -Recurse -File) {
    $dst = Join-Path $Out $f.FullName.Substring($root.Length + 1)
    New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
    $data = [IO.File]::ReadAllBytes($f.FullName)
    $obj = switch ($f.Extension.ToLowerInvariant()) {
      '.ytd' { New-Object CodeWalker.GameFiles.YtdFile }
      '.ydr' { New-Object CodeWalker.GameFiles.YdrFile }
      '.ydd' { New-Object CodeWalker.GameFiles.YddFile }
      '.yft' { New-Object CodeWalker.GameFiles.YftFile }
      '.ypt' { New-Object CodeWalker.GameFiles.YptFile }
      default { $null }
    }
    if (-not $obj) { [IO.File]::WriteAllBytes($dst, $data); $copied++; continue }
    try {
      $obj.Load($data)
      $bytes = $obj.Save()
      if (-not $bytes -or $bytes.Length -lt 16) { throw 'Save returned nothing' }
      [IO.File]::WriteAllBytes($dst, $bytes)
      $ok++
      "$($f.Name) v$([BitConverter]::ToUInt32($data, 4)) -> v$([BitConverter]::ToUInt32($bytes, 4))"
    } catch {
      $failed++
      "FAIL $($f.Name): $($_.Exception.GetBaseException().Message)"
    }
  }
} finally {
  [CodeWalker.GameFiles.RpfManager]::IsGen9 = $was
}
"converted $ok, copied $copied, failed $failed"
if ($failed -gt 0) { exit 1 }
exit 0

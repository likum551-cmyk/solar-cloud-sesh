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
#   cw_gen9.ps1 -Mode gen9fix -CodeWalker <dir> -In <converted folder> -Out <same folder>
#       The two known Enhanced crash causes of converted cars (docs.sollumz.org "Asset conversion for GTAV Enhanced"), in place:
#       (1) every script_rt_* texture (a dashboard render target) is REMOVED from the .ytd files ("STRIPRT <file> <names>"):
#           a friend's Enhanced 1158 died ERR_GFX_STATE getting into the rs6 (2026-11-05) although its script_rt_dials_supergt
#           matched the vanilla one (512x512 A8R8G8B8, 1 level); the gen9 converter writes it as an ordinary texture. Without it
#           the dials just do not animate. (2) a .yft with over 128 shaders or a model with over 128 geometries (Enhanced's limit)
#           is reported "OVER <file> ..." for stage_packs to deal with. Exit 1 on any failure.
#
# <CodeWalker> = the folder holding CodeWalker.Core.dll (CodeWalker30_dev48 or newer; the converter is not in older builds).
# Exit 1 on any failure.
param(
  [Parameter(Mandatory)][ValidateSet('extract', 'convert', 'gen9fix')][string]$Mode,
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

if ($Mode -eq 'gen9fix') {
  [CodeWalker.GameFiles.RpfManager]::IsGen9 = $true
  $root = (Resolve-Path $In).Path.TrimEnd('\')
  $ytds = 0; $yfts = 0; $stripped = 0
  foreach ($f in Get-ChildItem -Path $root -Recurse -File -Filter *.ytd) {
    $ytds++
    $y = New-Object CodeWalker.GameFiles.YtdFile
    $y.Load([IO.File]::ReadAllBytes($f.FullName))
    $all = @($y.TextureDict.Textures.data_items)
    $keep = New-Object 'System.Collections.Generic.List[CodeWalker.GameFiles.Texture]'
    $gone = @()
    foreach ($t in $all) { if ($t.Name -like 'script_rt*') { $gone += $t.Name } else { $keep.Add($t) } }
    if (-not $gone.Count) { continue }
    if (-not $keep.Count) { "FAIL $($f.Name): only script_rt textures"; exit 1 }
    $y.TextureDict.BuildFromTextureList($keep)
    $bytes = $y.Save()
    $back = New-Object CodeWalker.GameFiles.YtdFile
    $back.Load($bytes)
    $names = @($back.TextureDict.Textures.data_items | ForEach-Object { $_.Name })
    if ($names.Count -ne $keep.Count -or ($names | Where-Object { $_ -like 'script_rt*' })) { "FAIL $($f.Name): the rewritten dictionary reads back $($names.Count) of $($keep.Count)"; exit 1 }
    [IO.File]::WriteAllBytes($f.FullName, $bytes)
    $stripped++
    "STRIPRT $($f.Name) $($gone -join ',')"
  }
  foreach ($f in Get-ChildItem -Path $root -Recurse -File -Filter *.yft) {
    $yfts++
    $y = New-Object CodeWalker.GameFiles.YftFile
    $y.Load([IO.File]::ReadAllBytes($f.FullName))
    $d = $y.Fragment.Drawable
    $sh = 0; if ($d -and $d.ShaderGroup -and $d.ShaderGroup.Shaders) { $sh = $d.ShaderGroup.Shaders.data_items.Count }
    $maxGeo = 0
    if ($d) { foreach ($lod in @($d.DrawableModels.High, $d.DrawableModels.Med, $d.DrawableModels.Low, $d.DrawableModels.VLow)) {
      if ($null -eq $lod) { continue }
      foreach ($m in $lod) { if ($m.Geometries.Length -gt $maxGeo) { $maxGeo = $m.Geometries.Length } } } }
    if ($sh -gt 128 -or $maxGeo -gt 128) { "OVER $($f.Name) shaders $sh largest-model geometries $maxGeo" }
  }
  "gen9fix: $ytds ytd ($stripped had script_rt textures removed), $yfts yft checked"
  if ($ytds + $yfts -eq 0) { exit 1 }
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

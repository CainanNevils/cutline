# Checks a release package (the .zip, or an unpacked folder) for personal information before it
# is published: this PC's Windows user name and computer name, the git name and e-mail addresses
# (repo and global; all looked up when it runs, so nothing personal is stored in the repo),
# user-profile paths (the Users folder, AppData) and e-mail addresses in Cutline's own files.
# Looks inside the programs too, as plain and UTF-16 text. Exit code 1 if anything is found.
#
# Third-party DLLs (Qt, FFmpeg, the VC++ runtime) contain their authors' contact addresses and
# build paths, so the e-mail and generic path checks cover only Cutline's own files; the checks
# for this PC's names and addresses cover every file.
# Build-folder paths such as D:\cutline\... inside the programs are not personal: not reported.
#
# Usage: .\scripts\scan-package.ps1 dist\Cutline-0.1.0-win64.zip
# -Allow: text that may appear even though it contains a checked name (the public repository's
# address, e.g. "github.com/<owner>/" holds the GitHub user name); removed before checking.
param([Parameter(Mandatory)][string]$Path, [string[]]$Allow = @())
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$latin1 = [Text.Encoding]::GetEncoding(28591)  # one byte = one character, so binary files can be searched as text

# What to look for: label (shown) and value (never printed).
$needles = @()
function Add-Needle([string]$label, [string]$value) {
    if ($value -and $value.Length -ge 4) { $script:needles += [pscustomobject]@{ Label = $label; Value = $value.ToLowerInvariant() } }
}
Add-Needle 'the Windows user name' $env:USERNAME
Add-Needle 'the computer name' $env:COMPUTERNAME
foreach ($scope in @(), @('--global')) {
    $email = (git config @scope user.email 2>$null)
    if ($email -and $email -notlike '*.invalid') {
        Add-Needle 'a git e-mail address' $email
        Add-Needle 'a git e-mail name' ($email -split '@')[0]
    }
    $gname = (git config @scope user.name 2>$null)
    if ($gname -and $gname -ne 'Cutline Developer') { Add-Needle 'a git user name' $gname }
}
# Generic profile paths: Cutline's own files only (Qt's DLLs carry their build machine's
# Users\qt folder); this PC's user name above is still checked in every file. (Assembled here so
# this file, which is published, does not contain the patterns it looks for.)
$u = 'us' + 'ers'; $ad = 'app' + 'data'
$ownOnly = @("c:\$u\", "c:/$u/", "\$ad\")

function Get-Entries([string]$p) {
    $item = Get-Item -LiteralPath $p
    if ($item.PSIsContainer) {
        $base = $item.FullName.TrimEnd('\')
        foreach ($f in Get-ChildItem -LiteralPath $p -Recurse -File) {
            [pscustomobject]@{ Name = $f.FullName.Substring($base.Length).TrimStart('\'); Bytes = [IO.File]::ReadAllBytes($f.FullName) }
        }
    } else {
        $zip = [IO.Compression.ZipFile]::OpenRead($item.FullName)
        try {
            foreach ($e in $zip.Entries) {
                if ($e.Length -eq 0) { continue }
                $ms = New-Object IO.MemoryStream
                $s = $e.Open(); $s.CopyTo($ms); $s.Close()
                [pscustomobject]@{ Name = $e.FullName; Bytes = $ms.ToArray() }
            }
        } finally { $zip.Dispose() }
    }
}

$flagged = 0
$count = 0
foreach ($e in Get-Entries $Path) {
    $count++
    $text = $latin1.GetString($e.Bytes).ToLowerInvariant()
    foreach ($a in $Allow) {
        $al = $a.ToLowerInvariant()
        $text = $text.Replace($al, ' ').Replace((($al.ToCharArray() | ForEach-Object { "$_`0" }) -join ''), ' ')
    }
    $hits = New-Object System.Collections.Generic.List[string]
    foreach ($n in $needles) {
        $wide = (($n.Value.ToCharArray() | ForEach-Object { "$_`0" }) -join '')  # UTF-16: a, NUL, b, NUL...
        if ($text.Contains($n.Value)) { $hits.Add("$($n.Label) (as text)") }
        if ($text.Contains($wide)) { $hits.Add("$($n.Label) (as UTF-16 text)") }
    }
    $leaf = ($e.Name -split '[\\/]')[-1]
    $thirdParty = $leaf -like '*.dll' -or $leaf -in 'ffmpeg.exe', 'ffprobe.exe' -or $e.Name -like 'third_party*'  # (vendored headers in the source)
    if (-not $thirdParty) {
        foreach ($v in $ownOnly) {
            $wide = (($v.ToCharArray() | ForEach-Object { "$_`0" }) -join '')
            if ($text.Contains($v) -or $text.Contains($wide)) { $hits.Add('a user-profile path') }
        }
        foreach ($m in [regex]::Matches($text, '[a-z0-9._%+-]+@[a-z0-9-]+(\.[a-z0-9-]+)*\.(com|net|org|io|me|co|uk|de|dev)\b')) {
            $hits.Add('an e-mail address')
        }
    }
    $hits = @($hits | Select-Object -Unique)
    if ($hits.Count) {
        $flagged++
        Write-Host ('  FOUND  {0,-40} {1}' -f $e.Name, ($hits -join '; ')) -ForegroundColor Red
    }
}
if (-not $count) { Write-Host 'scan-package: nothing to scan' -ForegroundColor Red; exit 1 }
if ($flagged) {
    Write-Host "scan-package: personal information found in $flagged file(s). Do not publish this." -ForegroundColor Red
    exit 1
}
Write-Host "scan-package: $count files checked, no personal information found." -ForegroundColor Green
exit 0

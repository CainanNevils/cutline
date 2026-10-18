# Publishes a new version of Cutline. See docs\RELEASING.md.
#
# Usage:  .\scripts\release.ps1 0.2.0 -Notes "What changed"
#         .\scripts\release.ps1 0.2.0 -DryRun      everything except publishing (see below)
#
# Two repos: the private working repo (origin, all history) and the public repo (GPL source: one
# commit per release, plus the release zips). It:
#   1. checks you're on a clean main that matches origin;
#   2. sets the version in CMakeLists.txt, builds and runs every test suite (stops on failure);
#   3. packages the zip + .sha256 and scans it for personal information;
#   4. builds the public source snapshot (the release commit minus scripts\public-exclude.txt),
#      scans it for personal information and for the words in scripts\release-banned-words.txt;
#   5. downloads the source of the FFmpeg version the package ships (the GPL needs it published);
#   6. commits "Release vX.Y.Z", tags it and pushes to the private repo;
#   7. pushes the snapshot as a new commit + tag to the public repo and creates the release there
#      with the zip, its checksum and the FFmpeg source.
#
# -DryRun does 2-5 (into dist\dryrun\), undoes the version edit and stops: nothing is committed,
# tagged, pushed or uploaded, and GitHub's API is not used. Works from any branch. The snapshot is
# made from the last commit, so commit what you want in it first.
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$Notes = '',
    [string]$NotesFile = '',  # the notes from a file (multi-line notes survive any shell)
    [string]$PublicRepo = 'CainanNevils/cutline',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

function Fail($msg) { Write-Host "release: $msg" -ForegroundColor Red; exit 1 }

$gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
if (-not $gh) { $gh = 'C:\Program Files\GitHub CLI\gh.exe' }
if (-not $DryRun) {
    if (-not (Test-Path $gh)) { Fail 'GitHub CLI not found. Install it: winget install GitHub.cli' }
    & $gh auth status *> $null
    if ($LASTEXITCODE) { Fail 'Not logged in to GitHub. Run: gh auth login --web' }
}

# Words that must never be published (kept in a file that is itself not published).
$bannedFile = Join-Path $PSScriptRoot 'release-banned-words.txt'
$banned = @(Get-Content $bannedFile | Where-Object { $_ -and -not $_.StartsWith('#') })
if (-not $banned.Count) { Fail "$bannedFile is missing or empty" }
$bannedRegex = '(?i)' + ($banned -join '|')
$exclude = @(Get-Content (Join-Path $PSScriptRoot 'public-exclude.txt') | Where-Object { $_ -and -not $_.StartsWith('#') })
# the public repository's address contains the GitHub user name: allowed there, flagged anywhere else
$owner = ($PublicRepo -split '/')[0]
$allow = @("github.com/$owner/", "$PublicRepo")

# --- checks ---------------------------------------------------------------
if ($Version -notmatch '^\d+\.\d+\.\d+$') { Fail "version must look like 1.2.3 (got '$Version')" }
if ($DryRun) {
    if (git status --porcelain -- CMakeLists.txt) { Fail 'CMakeLists.txt has uncommitted changes: commit or discard them first' }
} else {
    if ((git rev-parse --abbrev-ref HEAD) -ne 'main') { Fail 'switch to the main branch first (git checkout main)' }
    if (git status --porcelain) { Fail 'commit or discard your changes first (git status shows what)' }
    git fetch -q origin
    if ((git rev-parse main) -ne (git rev-parse origin/main)) { Fail 'main differs from the private repo: git push first' }
}
$cmake = Get-Content CMakeLists.txt -Raw
$current = [regex]::Match($cmake, 'project\(cutline VERSION ([0-9.]+)').Groups[1].Value
$released = @(git tag -l 'v*')
# the very first release may keep the current version; after that versions only go up
if ($released.Count -and [version]$Version -le [version]$current) { Fail "the new version must be higher than the current one ($current)" }
if (-not $released.Count -and [version]$Version -lt [version]$current) { Fail "the version must not be lower than the current one ($current)" }
if (git tag -l "v$Version") { Fail "tag v$Version already exists" }

# --- the release notes (public!): yours, or the commit titles since the last release ---
if ($NotesFile) { $Notes = ([IO.File]::ReadAllText((Resolve-Path $NotesFile))).Trim() }
if (-not $Notes) {
    $prev = if ($released.Count) { git describe --tags --abbrev=0 } else { $null }  # (no tags: a native error under 'Stop')
    $log = if ($prev) { git log --format='- %s' "$prev..HEAD" } else { @('- First release') }
    $Notes = ($log -join "`n")
}
if ($Notes -match $bannedRegex) { Fail 'the release notes contain a word from scripts\release-banned-words.txt. Pass your own -Notes.' }
$Notes += "`n`nNew install or update: download the zip, unzip it, double-click Install.cmd." +
    "`nCutline is free software (GNU GPL v3 or later). The source of this version is this repository at tag v$Version;" +
    ' the source of the bundled FFmpeg is attached below.'

# --- set the version (CMakeLists.txt -> exe properties, About box, zip name) ---
Write-Host "$(if ($DryRun) { 'DRY RUN: ' })Releasing $current -> $Version" -ForegroundColor Cyan
$changed = $Version -ne $current
if ($changed) {
    $cmake = $cmake -replace 'project\(cutline VERSION [0-9.]+', "project(cutline VERSION $Version"
    [IO.File]::WriteAllText((Join-Path $root 'CMakeLists.txt'), $cmake)  # UTF-8 without BOM
}

# The public source snapshot: the tree of a commit minus the excluded paths. Returns the tree id.
function New-PublicTree([string]$commit) {
    $idx = Join-Path ([IO.Path]::GetTempPath()) "cutline-public-index-$PID"
    $env:GIT_INDEX_FILE = $idx
    try {
        git read-tree $commit
        foreach ($p in $exclude) { git rm -q -r --cached --ignore-unmatch -- $p | Out-Null }
        return (git write-tree)
    } finally {
        Remove-Item Env:GIT_INDEX_FILE
        Remove-Item $idx -ErrorAction SilentlyContinue
    }
}

# Scans a snapshot tree: personal information (scan-package.ps1) and banned words.
function Test-PublicTree([string]$tree, [string]$outDir) {
    $zipPath = Join-Path $outDir "Cutline-$Version-public-source.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath }
    git archive --format=zip -o $zipPath $tree
    Write-Host 'Scanning the public source snapshot for personal information...' -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'scan-package.ps1') -Path $zipPath -Allow $allow
    if ($LASTEXITCODE) { throw 'the public source snapshot contains personal information (see above)' }
    $hits = @(git grep -I -l -i -E ($banned -join '|') $tree 2>$null)
    if ($hits.Count) { throw "the public source snapshot mentions a banned word in: $($hits -join ', ')" }
    Write-Host "public source snapshot: $((git ls-tree -r --name-only $tree | Measure-Object).Count) files, scans clean" -ForegroundColor Green
}

# The FFmpeg version inside the package and its source archive (GPL: published with each release).
function Get-FfmpegSource([string]$pkgDir, [string]$outDir) {
    $line = (& (Join-Path $pkgDir 'tools\ffmpeg.exe') -hide_banner -version 2>$null | Select-Object -First 1)
    $ver = [regex]::Match($line, 'ffmpeg version (\S+)').Groups[1].Value
    if (-not $ver) { throw 'could not read the bundled FFmpeg version' }
    $m = [regex]::Match($ver, '-g([0-9a-f]{7,40})')
    $ref = if ($m.Success) { $m.Groups[1].Value } else { $ver }  # a build of a commit, or a release tag
    $file = Join-Path $outDir "ffmpeg-$ver-source.tar.gz"
    if (-not (Test-Path $file)) {
        Write-Host "Downloading the FFmpeg source of $ver..." -ForegroundColor Cyan
        Invoke-WebRequest "https://github.com/FFmpeg/FFmpeg/archive/$ref.tar.gz" -OutFile $file -UseBasicParsing
    }
    if ((Get-Item $file).Length -lt 5MB) { throw "the FFmpeg source download looks wrong: $file" }
    return $file
}

$outDir = Join-Path $root $(if ($DryRun) { 'dist\dryrun' } else { 'dist' })
New-Item -ItemType Directory -Force $outDir | Out-Null
$zip = Join-Path $outDir "Cutline-$Version-win64.zip"
try {
    # --- build, test, package ---------------------------------------------
    & (Join-Path $PSScriptRoot 'test.ps1')  # builds release and runs every test suite (throws on failure)
    & (Join-Path $PSScriptRoot 'package.ps1') -NoBuild -OutDir $outDir | Out-Host
    if (-not (Test-Path $zip) -or -not (Test-Path "$zip.sha256")) { throw 'package missing' }
    $pkgDir = Join-Path $outDir "Cutline-$Version-win64"
    $exeVersion = (Get-Item (Join-Path $pkgDir 'Cutline.exe')).VersionInfo.ProductVersion
    if ($exeVersion -ne $Version) { throw "the built exe says $exeVersion, expected $Version" }
    Write-Host 'Scanning the package for personal information...' -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'scan-package.ps1') -Path $zip -Allow $allow
    if ($LASTEXITCODE) { throw 'the package scan found personal information (see above)' }
    # --- the public source snapshot and the FFmpeg source ---------------------
    $tree = New-PublicTree 'HEAD'
    Test-PublicTree $tree $outDir
    $ffSource = Get-FfmpegSource $pkgDir $outDir
} catch {
    if ($changed) { git checkout -- CMakeLists.txt }
    Fail "build/test/package/scan failed, nothing was published: $_"
}

if ($DryRun) {
    if ($changed) { git checkout -- CMakeLists.txt }
    Write-Host ''
    Write-Host "DRY RUN finished: Cutline $Version would be released as is." -ForegroundColor Green
    Write-Host "  package: $zip ($('{0:N1}' -f ((Get-Item $zip).Length / 1MB)) MB), tests passed, scan clean"
    Write-Host "  public source snapshot: dist\dryrun\Cutline-$Version-public-source.zip (left out: $($exclude -join ', '))"
    Write-Host "  FFmpeg source to attach: $ffSource ($('{0:N1}' -f ((Get-Item $ffSource).Length / 1MB)) MB)"
    Write-Host '  release notes that would be published:'
    $Notes -split "`n" | ForEach-Object { Write-Host "    $_" }
    Write-Host 'Nothing was committed, tagged, pushed or uploaded.'
    exit 0
}

# --- commit, tag, push (private repo) -------------------------------------
if ($changed) { git commit -q -am "Release v$Version" }
git tag -a "v$Version" -m "Cutline $Version"
git push -q origin main "v$Version"
if ($LASTEXITCODE) { Fail 'push to the private repo failed; the release commit and tag are local only (nothing published yet)' }

# --- the public repo: one source commit per release ----------------------
$publicUrl = "https://github.com/$PublicRepo.git"
& $gh repo view $PublicRepo --json name *> $null
if ($LASTEXITCODE) { Fail "public repo $PublicRepo not found (see docs\RELEASING.md, first release)" }
if (-not (git remote | Where-Object { $_ -eq 'public' })) { git remote add public $publicUrl } else { git remote set-url public $publicUrl }
git fetch -q public
$parent = git rev-parse --verify -q public/main
$tree = New-PublicTree 'HEAD'  # (now including the version bump)
$msgFile = Join-Path ([IO.Path]::GetTempPath()) "cutline-public-msg-$Version.txt"
[IO.File]::WriteAllText($msgFile, "Cutline $Version`n`nSource of the Cutline $Version release.`n")
$snapshot = if ($parent) { git commit-tree $tree -p $parent -F $msgFile } else { git commit-tree $tree -F $msgFile }
Remove-Item $msgFile -ErrorAction SilentlyContinue
git push -q public "${snapshot}:refs/heads/main" "${snapshot}:refs/tags/v$Version"
if ($LASTEXITCODE) { Fail "push to the public repo failed (the private repo already has v$Version). Retry: git push public ${snapshot}:refs/heads/main ${snapshot}:refs/tags/v$Version" }

# --- the release (public repo) --------------------------------------------
# Through a file: Windows PowerShell 5.1 mangles quotes and line breaks in long arguments to an .exe.
$notesFile = Join-Path ([IO.Path]::GetTempPath()) "cutline-release-notes-$Version.md"
[IO.File]::WriteAllText($notesFile, $Notes, (New-Object Text.UTF8Encoding($false)))
& $gh release create "v$Version" $zip "$zip.sha256" $ffSource -R $PublicRepo --verify-tag --title "Cutline $Version" --notes-file $notesFile
$uploadFailed = $LASTEXITCODE
Remove-Item $notesFile -ErrorAction SilentlyContinue
if ($uploadFailed) { Fail "upload failed. Fix it, then: gh release create v$Version `"$zip`" `"$zip.sha256`" `"$ffSource`" -R $PublicRepo --verify-tag --notes-file <a file with the notes>" }
Write-Host "Published Cutline ${Version}: https://github.com/$PublicRepo/releases/tag/v$Version" -ForegroundColor Green

# Run from a source checkout with Visual Studio C++ Build Tools installed.
# Compatible with Windows PowerShell 5.1 and PowerShell 7.
[CmdletBinding()]
param(
    [string]$InnoCompilerPath,
    [switch]$SkipCompile,
    [switch]$SkipInstaller
)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
$dist=Join-Path $repo 'dist'
$version='1.1.28'
$prefix='LightMon-'+$version
$work=Join-Path $repo ('build\release-'+[Guid]::NewGuid().ToString('N'))
$payload=Join-Path $work 'payload'
$sourceStage=Join-Path $work 'source'
$success=$false

function Copy-RequiredFile([string]$from,[string]$to) {
    if(-not (Test-Path -LiteralPath $from -PathType Leaf)){throw ('Required release file missing: '+$from)}
    $null=New-Item -ItemType Directory -Path (Split-Path -Parent $to) -Force
    Copy-Item -LiteralPath $from -Destination $to
}
function Copy-RequiredTree([string]$from,[string]$to) {
    if(-not (Test-Path -LiteralPath $from -PathType Container)){throw ('Required release directory missing: '+$from)}
    if(@(Get-ChildItem -LiteralPath $from -Force -Recurse|Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}).Count){throw ('Reparse point in release tree: '+$from)}
    $null=New-Item -ItemType Directory -Path $to -Force
    foreach($item in Get-ChildItem -LiteralPath $from -Force){Copy-Item -LiteralPath $item.FullName -Destination $to -Recurse}
}
function Assert-CleanTree([string]$root) {
    foreach($file in Get-ChildItem -LiteralPath $root -File -Force -Recurse){
        if($file.Name -match '(?i)(\.traffic$|\.log$|\.pdb$|\.obj$|\.env($|\.)|id_rsa|credentials|state-before|receipt\.json$)'){throw ('Private/runtime/build artifact in release: '+$file.Name)}
        if($file.Extension -in @('.cpp','.h','.rc','.ps1','.cmd','.md','.txt','.ini','.yml','.yaml','.json')){
            $text=[IO.File]::ReadAllText($file.FullName)
            if($text -match '(?i)[A-Z]:\\Users\\[^\\\s"'']+' -or $text -match 'S-1-5-21-\d+-\d+-\d+-\d+' -or $text -match '(ghp_|github_pat_)[A-Za-z0-9_]{20,}') {throw ('Machine path, SID, or token pattern in release text: '+$file.Name)}
        }
    }
}
function Write-Ascii([string]$path,[string[]]$lines){[IO.File]::WriteAllLines($path,$lines,[Text.Encoding]::ASCII)}

try {
    $null=New-Item -ItemType Directory -Path $work,$payload,$sourceStage,$dist -Force
    if(-not $SkipCompile){
        & (Join-Path $repo 'source\build.cmd')
        if($LASTEXITCODE -ne 0){throw ('Native build failed with exit code '+$LASTEXITCODE)}
    }
    $main=Join-Path $repo 'LightMon.exe'; $helper=Join-Path $repo 'LightMonCpu.exe'
    foreach($binary in @($main,$helper)){if(-not (Test-Path -LiteralPath $binary -PathType Leaf)){throw ('Built binary is missing: '+$binary)}}
    if((Get-Item -LiteralPath $main).VersionInfo.FileVersion -ne $version){throw 'Built main version does not match release version.'}
    $helperHash=(Get-FileHash -LiteralPath $helper -Algorithm SHA256).Hash
    $hashHeader=[IO.File]::ReadAllText((Join-Path $repo 'source\src\cpu_temperature_helper_hash.h'))
    $embeddedHash=([regex]::Matches($hashHeader,'0x([0-9A-Fa-f]{2})')|ForEach-Object {$_.Groups[1].Value}) -join ''
    if($embeddedHash -ine $helperHash){throw 'Source helper hash does not match the released helper. Rebuild both binaries together.'}

    # Source ZIP is assembled from an explicit public-file allowlist, never the workspace.
    foreach($name in @('README.md','LICENSE','THIRD_PARTY_NOTICES.md')){Copy-RequiredFile (Join-Path $repo $name) (Join-Path $sourceStage $name)}
    foreach($optional in @('README.en.md','CHANGELOG.md','CONTRIBUTING.md','SECURITY.md','.gitignore','.gitattributes')){if(Test-Path -LiteralPath (Join-Path $repo $optional)){Copy-RequiredFile (Join-Path $repo $optional) (Join-Path $sourceStage $optional)}}
    foreach($name in @('scripts','packaging','docs','LICENSES','third_party')){Copy-RequiredTree (Join-Path $repo $name) (Join-Path $sourceStage $name)}
    Copy-RequiredFile (Join-Path $repo 'source\build.cmd') (Join-Path $sourceStage 'source\build.cmd')
    Copy-RequiredFile (Join-Path $repo 'source\generate-helper-hash.ps1') (Join-Path $sourceStage 'source\generate-helper-hash.ps1')
    Copy-RequiredTree (Join-Path $repo 'source\src') (Join-Path $sourceStage 'source\src')
    if(Test-Path -LiteralPath (Join-Path $repo '.github\workflows')){Copy-RequiredTree (Join-Path $repo '.github\workflows') (Join-Path $sourceStage '.github\workflows')}
    Assert-CleanTree $sourceStage
    $sourceZip=Join-Path $dist ($prefix+'-Source.zip')
    Compress-Archive -Path (Join-Path $sourceStage '*') -DestinationPath $sourceZip -CompressionLevel Optimal -Force

    # Keep the same rebuildable source tree next to binaries in both distributions.
    Copy-RequiredTree $sourceStage $payload
    foreach($name in @('LightMon.exe','LightMonCpu.exe')){Copy-RequiredFile (Join-Path $repo $name) (Join-Path $payload $name)}
    Copy-RequiredFile (Join-Path $repo 'packaging\LightMon.ini') (Join-Path $payload 'LightMon.ini')
    if(-not (Test-Path -LiteralPath (Join-Path $payload 'scripts\Configure-CpuTemperature.ps1') -PathType Leaf)){throw 'Optional CPU temperature setup script missing from release.'}
    Write-Ascii (Join-Path $payload 'LightMonCpu.sha256') @($helperHash+'  LightMonCpu.exe')
    Assert-CleanTree $payload
    $portable=Join-Path $dist ($prefix+'-Portable-x64.zip')
    Compress-Archive -Path (Join-Path $payload '*') -DestinationPath $portable -CompressionLevel Optimal -Force

    $assets=@($sourceZip,$portable)
    if(-not $SkipInstaller){
        if([string]::IsNullOrEmpty($InnoCompilerPath)){
            $command=Get-Command ISCC.exe -ErrorAction SilentlyContinue
            if($command){$InnoCompilerPath=$command.Source}
            else {
                foreach($candidate in @((Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),(Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'))){if(Test-Path -LiteralPath $candidate -PathType Leaf){$InnoCompilerPath=$candidate;break}}
            }
        }
        if([string]::IsNullOrEmpty($InnoCompilerPath) -or -not (Test-Path -LiteralPath $InnoCompilerPath -PathType Leaf)){throw 'Inno Setup compiler not found. Pass -InnoCompilerPath or use -SkipInstaller for ZIP releases.'}
        & $InnoCompilerPath ('/DAppVersion='+$version) ('/DPayloadRoot='+$payload) ('/DReleaseOutputDir='+$dist) (Join-Path $repo 'packaging\LightMon.iss')
        if($LASTEXITCODE -ne 0){throw ('Inno Setup build failed with exit code '+$LASTEXITCODE)}
        $setup=Join-Path $dist ($prefix+'-Setup-x64.exe')
        if(-not (Test-Path -LiteralPath $setup -PathType Leaf)){throw 'Expected setup output was not created.'}
        $assets+=@($setup)
    }
    $hashes=@(foreach($asset in $assets){(Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash+'  '+[IO.Path]::GetFileName($asset)})
    Write-Ascii (Join-Path $dist 'SHA256SUMS.txt') $hashes
    $success=$true
    $assets|ForEach-Object {Get-Item -LiteralPath $_}|Select-Object Name,Length
    Write-Output ('Checksums: '+(Join-Path $dist 'SHA256SUMS.txt'))
} finally {
    # Only delete this invocation's verified generated staging directory.
    if($success -and (Test-Path -LiteralPath $work)){
        $resolved=(Resolve-Path -LiteralPath $work).Path
        $expectedPrefix=$repo+'\build\release-'
        if(-not $resolved.StartsWith($expectedPrefix,[StringComparison]::OrdinalIgnoreCase) -or (Split-Path -Leaf $resolved) -notmatch '^release-[a-f0-9]{32}$'){throw 'Refusing to clean an unexpected staging path.'}
        if((Get-Item -LiteralPath $resolved -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Refusing to clean a reparse-point staging directory.'}
        if(@(Get-ChildItem -LiteralPath $resolved -Force -Recurse|Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}).Count){throw 'Refusing to clean a staging tree containing reparse points.'}
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Install','Remove','Status')][string]$Action = 'Status',
    [string]$ExpectedUserSid = '',
    [string]$ExpectedHelperSha256 = '',
    [string]$ExpectedScriptSha256 = ''
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# Deliberately no arbitrary helper, destination, task name, or command argument.
# This version's C++ client validates these same fixed paths and task action.
$protectedDirectory = 'C:\Program Files\LightMon'
$protectedHelper = Join-Path $protectedDirectory 'LightMonCpu.exe'
$markerPath = Join-Path $protectedDirectory 'CpuTemperature-Setup.json'
$taskArguments = '--parent $(Arg0) --parent-start $(Arg1) --mapping "$(Arg2)"'
$markerKind = 'LightMon.CpuTemperature.Setup.v1'
$taskDescription = 'LightMon CPU temperature reader; managed by Configure-CpuTemperature.ps1 v1.'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$userSid = $identity.User.Value
$taskName = 'LightMon CPU Temperature ' + $userSid
$isElevated = ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$packageDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sourceHelper = Join-Path $packageDirectory 'LightMonCpu.exe'
$hashPath = Join-Path $packageDirectory 'LightMonCpu.sha256'

function Assert-PlainPath([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    while ($null -ne $item) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw ('Reparse point refused: ' + $item.FullName) }
        if ($item -is [IO.FileInfo]) { $item = $item.Directory } else { $item = $item.Parent }
    }
}
function Assert-ProtectedObject([string]$Path, [switch]$InheritedSystemPath) {
    Assert-PlainPath $Path
    $acl = Get-Acl -LiteralPath $Path
    $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
    $trustedOwners = @('S-1-5-18','S-1-5-32-544')
    if ($InheritedSystemPath) { $trustedOwners += 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464' } # TrustedInstaller
    if ($owner -notin $trustedOwners) { throw ('Unexpected protected owner: ' + $Path) }
    if (-not $InheritedSystemPath -and -not $acl.AreAccessRulesProtected) { throw ('Inherited permissions refused: ' + $Path) }
    $security = New-Object Security.AccessControl.RawSecurityDescriptor($acl.GetSecurityDescriptorBinaryForm(),0)
    if ($null -eq $security.DiscretionaryAcl -or $security.DiscretionaryAcl.Count -eq 0) { throw ('Missing protected DACL: ' + $Path) }
    foreach ($ace in $security.DiscretionaryAcl) {
        # System directories can contain Creator Owner rules that apply only to
        # future children, not the existing system directory being validated.
        if ($InheritedSystemPath -and (([int]$ace.AceFlags -band [int][Security.AccessControl.AceFlags]::InheritOnly) -ne 0)) { continue }
        if ($ace -isnot [Security.AccessControl.CommonAce] -or $ace.IsCallback -or
            $ace.AceQualifier -ne [Security.AccessControl.AceQualifier]::AccessAllowed) { throw ('Unexpected protected ACL: ' + $Path) }
        if ($ace.SecurityIdentifier.Value -notin $trustedOwners) {
            $unsignedMask = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$ace.AccessMask),0)
            if (([int64]$unsignedMask -band (-bnot [int64]2685534377)) -ne 0) { throw ('Writable protected path refused: ' + $Path) }
        }
    }
}
function Set-ProtectedDirectoryAcl([string]$Path) {
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)')
    Set-Acl -LiteralPath $Path -AclObject $acl
}
function Set-ProtectedFileAcl([string]$Path) {
    $acl = New-Object Security.AccessControl.FileSecurity
    $acl.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)')
    Set-Acl -LiteralPath $Path -AclObject $acl
}
function Get-OptionalTask($Folder, [string]$Name) {
    try { return $Folder.GetTask($Name) }
    catch {
        $failure = $_.Exception
        while ($null -ne $failure) {
            if ($failure.HResult -eq -2147024894) { return $null }
            $failure = $failure.InnerException
        }
        throw
    }
}
function Assert-OwnedTask($Task) {
    if ($Task.Definition.RegistrationInfo.Description -ne $taskDescription -or
        $Task.Definition.Actions.Count -ne 1 -or $Task.Definition.Triggers.Count -ne 0) { throw 'An unrelated task occupies the CPU-temperature task name.' }
    $actionDefinition = $Task.Definition.Actions.Item(1)
    if ($actionDefinition.Type -ne 0 -or $actionDefinition.Path -ne $protectedHelper -or
        $actionDefinition.WorkingDirectory -ne $protectedDirectory -or
        $actionDefinition.Arguments -cne $taskArguments -or
        $Task.Definition.Principal.LogonType -ne 3 -or $Task.Definition.Principal.RunLevel -ne 1) { throw 'Existing temperature task differs; refusing to replace or remove it.' }
    # Parse service-provided XML with external entities and DTDs disabled.
    $readerSettings = New-Object Xml.XmlReaderSettings
    $readerSettings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
    $readerSettings.XmlResolver = $null
    $reader = [Xml.XmlReader]::Create((New-Object IO.StringReader($Task.Xml)),$readerSettings)
    try {
        $document = New-Object Xml.XmlDocument
        $document.XmlResolver = $null
        $document.Load($reader)
        $namespaces = New-Object Xml.XmlNamespaceManager($document.NameTable)
        $namespaces.AddNamespace('t','http://schemas.microsoft.com/windows/2004/02/mit/task')
        $users = $document.SelectNodes('/t:Task/t:Principals/t:Principal/t:UserId',$namespaces)
        if ($users.Count -ne 1 -or $users[0].InnerText -ne $userSid) { throw 'Existing task principal differs from this user.' }
    } finally { $reader.Dispose() }
    $security = New-Object Security.AccessControl.RawSecurityDescriptor($Task.GetSecurityDescriptor(7))
    if ($security.Owner.Value -notin @('S-1-5-18','S-1-5-32-544')) { throw 'Existing task is not administrator owned.' }
    if ($null -eq $security.DiscretionaryAcl -or $security.DiscretionaryAcl.Count -eq 0 -or
        ($security.ControlFlags -band [Security.AccessControl.ControlFlags]::DiscretionaryAclProtected) -eq 0) { throw 'Existing task has no protected DACL.' }
    foreach ($ace in $security.DiscretionaryAcl) {
        if ($ace -isnot [Security.AccessControl.CommonAce] -or $ace.IsCallback -or
            $ace.AceQualifier -ne [Security.AccessControl.AceQualifier]::AccessAllowed) { throw 'Existing task has an unexpected ACL.' }
        if ($ace.SecurityIdentifier.Value -notin @('S-1-5-18','S-1-5-32-544')) {
            $unsignedMask = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$ace.AccessMask),0)
            # Accept either generic GR/GX or their service-expanded read/execute rights.
            if ($ace.SecurityIdentifier.Value -ne $userSid -or (([int64]$unsignedMask -band (-bnot [int64]2685534377)) -ne 0)) { throw 'Existing task grants unexpected non-administrator permissions.' }
        }
    }
}
function Read-OwnedMarker {
    Assert-ProtectedObject $protectedDirectory
    Assert-ProtectedObject $markerPath
    Assert-ProtectedObject $protectedHelper
    $marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
    if ($marker.kind -ne $markerKind -or $marker.user_sid -ne $userSid -or
        $marker.helper_sha256 -notmatch '^[A-Fa-f0-9]{64}$' -or
        (Get-FileHash -LiteralPath $protectedHelper -Algorithm SHA256).Hash -ne $marker.helper_sha256) { throw 'Protected helper ownership record does not match; refusing changes.' }
    return $marker
}

if ($ExpectedUserSid -and $ExpectedUserSid -ne $userSid) { throw 'UAC must use the same signed-in administrator account. A different administrator account is not supported.' }
if ($ExpectedScriptSha256 -and (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash -ne $ExpectedScriptSha256) { throw 'Setup script changed during elevation.' }
if ([Environment]::Is64BitOperatingSystem -ne $true -or [Environment]::Is64BitProcess -ne $true) { throw 'Use 64-bit Windows PowerShell on Windows 11 x64.' }

if ($Action -eq 'Status') {
    $scheduler = New-Object -ComObject Schedule.Service
    $scheduler.Connect()
    $task = Get-OptionalTask ($scheduler.GetFolder('\')) $taskName
    $owned = $false
    $detail = 'Not configured by this setup script.'
    if ($null -ne $task) {
        try { Assert-OwnedTask $task; $null = Read-OwnedMarker; $owned = $true; $detail = 'Managed task and protected helper verified.' }
        catch { $detail = $_.Exception.Message }
    }
    [pscustomobject]@{action='Status';task_exists=($null -ne $task);managed_configuration=$owned;detail=$detail;cpu_scope='AMD Family 19h Model 61h';task_name=$taskName}
    return
}

# Lock the script/source while UAC is pending. The elevated process additionally
# compares hashes and the original SID; no password or token is collected.
if (-not $isElevated) {
    Assert-PlainPath $PSCommandPath
    $scriptLock = [IO.File]::Open($PSCommandPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $helperLock = $null
    try {
        $scriptHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
        $helperHash = ''
        if ($Action -eq 'Install') {
            Assert-PlainPath $sourceHelper
            $helperLock = [IO.File]::Open($sourceHelper,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
            $helperHash = (Get-FileHash -LiteralPath $sourceHelper -Algorithm SHA256).Hash
        }
        $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Action ' + $Action + ' -ExpectedUserSid ' + $userSid + ' -ExpectedScriptSha256 ' + $scriptHash
        if ($helperHash) { $arguments += ' -ExpectedHelperSha256 ' + $helperHash }
        $powerShell = Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell\v1.0\powershell.exe'
        $child = Start-Process -FilePath $powerShell -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru -Wait
        if ($child.ExitCode -ne 0) { throw ('CPU-temperature setup did not complete (exit ' + $child.ExitCode + '). Run from an elevated PowerShell with the same user to read its error. No driver was installed.') }
        Write-Output 'CPU-temperature setup completed. Enable the automatic CPU-temperature option in the ordinary LightMon window, or disable it after removing the task.'
    } finally { if ($null -ne $helperLock) { $helperLock.Dispose() }; $scriptLock.Dispose() }
    return
}

foreach ($path in @('C:\','C:\Program Files')) { Assert-PlainPath $path }
$configuredProgramFiles = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion' -Name ProgramFilesDir).ProgramFilesDir
if ($configuredProgramFiles -ne 'C:\Program Files') { throw 'This version supports automatic CPU temperature only when Program Files is C:\Program Files. Other monitor features remain available.' }
Assert-ProtectedObject 'C:\Program Files' -InheritedSystemPath
$scheduler = New-Object -ComObject Schedule.Service
$scheduler.Connect()
$root = $scheduler.GetFolder('\')
$existing = Get-OptionalTask $root $taskName
if ($null -ne $existing) { Assert-OwnedTask $existing }
$marker = $null
if (Test-Path -LiteralPath $protectedDirectory) { $marker = Read-OwnedMarker }
if ($null -ne $existing -and $null -eq $marker) { throw 'Task exists without the setup ownership record.' }

if ($Action -eq 'Remove') {
    if ($null -eq $existing -and $null -eq $marker) { Write-Output 'No managed CPU-temperature setup exists.'; return }
    if ($null -ne $existing) {
        if ($existing.GetInstances(0).Count -ne 0) { throw 'Exit LightMon before removing its CPU-temperature setup.' }
        $root.DeleteTask($taskName,0)
    }
    $others = @($root.GetTasks(1) | Where-Object { $_.Name -like 'LightMon CPU Temperature *' })
    if ($others.Count -ne 0) { throw 'Another CPU-temperature task exists. This user task was removed; shared files have been retained.' }
    if ($null -ne $marker) {
        # Exact files only. Never recursively remove a protected program folder.
        Remove-Item -LiteralPath $protectedHelper -Force
        Remove-Item -LiteralPath $markerPath -Force
        if (@(Get-ChildItem -LiteralPath $protectedDirectory -Force).Count -eq 0) { Remove-Item -LiteralPath $protectedDirectory }
    }
    Write-Output 'Managed CPU-temperature task and helper removed. Disable automatic CPU temperature in LightMon. PawnIO, program settings, and other tasks were not changed.'
    return
}

$cpu = Get-ItemProperty -LiteralPath 'HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0'
if ($cpu.VendorIdentifier -ne 'AuthenticAMD' -or $cpu.Identifier -notmatch '\bFamily\s+25\s+Model\s+97\b') { throw 'CPU temperature supports only AMD Family 19h Model 61h in this version. Other monitor features remain available.' }
if (-not (Test-Path -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\PawnIO') -or
    -not (Test-Path -LiteralPath 'C:\Program Files\PawnIO\PawnIOLib.dll' -PathType Leaf)) { throw 'PawnIO is not installed. Review https://pawnio.eu/ and install its official driver separately if desired. This script does not download or install drivers.' }
Assert-ProtectedObject 'C:\Program Files\PawnIO' -InheritedSystemPath
Assert-ProtectedObject 'C:\Program Files\PawnIO\PawnIOLib.dll' -InheritedSystemPath
Assert-PlainPath $sourceHelper
Assert-PlainPath $hashPath
$manifest = (Get-Content -LiteralPath $hashPath -Raw).Trim()
if ($manifest -notmatch '^([A-Fa-f0-9]{64})(?:\s+\*?LightMonCpu\.exe)?$') { throw 'Invalid LightMonCpu.sha256; obtain the matching complete package.' }
$expectedHash = $Matches[1].ToUpperInvariant()
$sourceLock = [IO.File]::Open($sourceHelper,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try {
    if ((Get-FileHash -LiteralPath $sourceHelper -Algorithm SHA256).Hash -ne $expectedHash -or
        ($ExpectedHelperSha256 -and $ExpectedHelperSha256 -ne $expectedHash)) { throw 'Helper does not match the release SHA256; obtain the matching complete package.' }
    if ($null -ne $existing -and $existing.GetInstances(0).Count -ne 0) { throw 'Exit LightMon before updating its protected CPU helper.' }
    if ($null -eq $marker) {
        [void][IO.Directory]::CreateDirectory($protectedDirectory)
        Set-ProtectedDirectoryAcl $protectedDirectory
    }
    Assert-ProtectedObject $protectedDirectory
    $temporaryHelper = Join-Path $protectedDirectory ('LightMonCpu-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        Copy-Item -LiteralPath $sourceHelper -Destination $temporaryHelper
        Set-ProtectedFileAcl $temporaryHelper
        if ((Get-FileHash -LiteralPath $temporaryHelper -Algorithm SHA256).Hash -ne $expectedHash) { throw 'Copied helper hash differs.' }
        if (Test-Path -LiteralPath $protectedHelper) { [IO.File]::Replace($temporaryHelper,$protectedHelper,$null) }
        else { [IO.File]::Move($temporaryHelper,$protectedHelper) }
        Set-ProtectedFileAcl $protectedHelper
    } finally { if (Test-Path -LiteralPath $temporaryHelper) { Remove-Item -LiteralPath $temporaryHelper -Force } }
    $record = [ordered]@{kind=$markerKind;user_sid=$userSid;helper_sha256=$expectedHash}
    [IO.File]::WriteAllText($markerPath,($record | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
    Set-ProtectedFileAcl $markerPath
    $definition = $scheduler.NewTask(0)
    $definition.RegistrationInfo.Author = 'LightMon'
    $definition.RegistrationInfo.Description = $taskDescription
    $definition.Principal.UserId = $userSid
    $definition.Principal.LogonType = 3
    $definition.Principal.RunLevel = 1
    $definition.Settings.Enabled = $true
    $definition.Settings.AllowDemandStart = $true
    $definition.Settings.DisallowStartIfOnBatteries = $false
    $definition.Settings.StopIfGoingOnBatteries = $false
    $definition.Settings.ExecutionTimeLimit = 'PT0S'
    $definition.Settings.MultipleInstances = 0
    $definition.Settings.StartWhenAvailable = $false
    $definition.Settings.Hidden = $false
    $definition.Settings.RunOnlyIfIdle = $false
    $actionDefinition = $definition.Actions.Create(0)
    $actionDefinition.Path = $protectedHelper
    $actionDefinition.Arguments = $taskArguments
    $actionDefinition.WorkingDirectory = $protectedDirectory
    $taskSecurity = 'O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;' + $userSid + ')'
    $registrationFlags = if ($null -ne $existing) { 20 } else { 18 } # UPDATE/CREATE + DONT_ADD_PRINCIPAL_ACE
    $registered = $root.RegisterTaskDefinition($taskName,$definition,$registrationFlags,$userSid,$null,3,$taskSecurity)
    Assert-OwnedTask $registered
    $null = Read-OwnedMarker
    Write-Output 'CPU-temperature task configured. In LightMon, right-click the values and enable automatic CPU-temperature reading. Main LightMon remains at ordinary privileges.'
} finally { $sourceLock.Dispose() }

# Windows PowerShell 5.1 and PowerShell 7. Current user only; never elevates.
[CmdletBinding()]
param(
    [ValidateSet('Enable','Disable','Status')][string]$Action = 'Status',
    [string]$InstallDirectory = ''
)
$ErrorActionPreference = 'Stop'
if([string]::IsNullOrEmpty($InstallDirectory)) { $InstallDirectory=Split-Path -Parent $PSScriptRoot }
$installRoot = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
$exe = Join-Path $installRoot 'LightMon.exe'
$arguments = '--lightmon-task-start'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$sid = $identity.User.Value
$taskName = 'LightMon Monitor ' + $sid

function Get-PrincipalSid($task) {
    # COM can expose a display name (including an unresolvable Microsoft-account
    # alias). The registered XML preserves the canonical principal SID.
    $value = [string]([xml]$task.Xml).Task.Principals.Principal.UserId
    if($value -match '^S-1-') { return $value }
    try { return ([Security.Principal.NTAccount]::new($value)).Translate([Security.Principal.SecurityIdentifier]).Value }
    catch { return '' }
}
function Test-OwnedTask($task) {
    if($null -eq $task) { return $false }
    $definition = $task.Definition
    if((Get-PrincipalSid $task) -ne $sid -or $definition.Principal.LogonType -ne 3 -or $definition.Principal.RunLevel -ne 0 -or $definition.Actions.Count -ne 1) { return $false }
    $entry = $definition.Actions.Item(1)
    return $entry.Type -eq 0 -and [string]::Equals([string]$entry.Path,$exe,[StringComparison]::OrdinalIgnoreCase) -and $entry.Arguments -ceq $arguments -and [string]::Equals([string]$entry.WorkingDirectory,$installRoot,[StringComparison]::OrdinalIgnoreCase)
}
function Remove-OwnedLegacyRun {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run',$true)
    if($null -eq $key) { return }
    try {
        $value = $key.GetValue('LightMon',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        if($null -ne $value -and $key.GetValueKind('LightMon') -eq [Microsoft.Win32.RegistryValueKind]::String -and [string]::Equals([string]$value,('"'+$exe+'"'),[StringComparison]::OrdinalIgnoreCase)) { $key.DeleteValue('LightMon',$false) }
    } finally { $key.Dispose() }
}
function Assert-Installed($task) {
    if(-not (Test-OwnedTask $task)) { throw 'Installed task ownership verification failed.' }
    $d=$task.Definition; $s=$d.Settings
    if($d.Triggers.Count -ne 2 -or -not $s.Enabled -or -not $s.AllowDemandStart -or -not $s.StartWhenAvailable -or $s.MultipleInstances -ne 2 -or $s.ExecutionTimeLimit -ne 'PT0S' -or $s.DisallowStartIfOnBatteries -or $s.StopIfGoingOnBatteries -or $s.RunOnlyIfIdle -or $s.RunOnlyIfNetworkAvailable -or $s.WakeToRun) { throw 'Installed settings verification failed.' }
    $logon=$null; $daily=$null
    foreach($trigger in $d.Triggers) {
        if($trigger.Id -ceq 'LightMonLogon') { $logon=$trigger }
        elseif($trigger.Id -ceq 'LightMonDailyAssurance') { $daily=$trigger }
        else { throw 'Unexpected task trigger.' }
    }
    if($null -eq $logon -or $logon.Type -ne 9 -or -not $logon.Enabled -or $logon.Delay -ne 'PT10S') { throw 'Logon trigger verification failed.' }
    $triggerUser=[string]([xml]$task.Xml).Task.Triggers.LogonTrigger.UserId
    $triggerSid=if($triggerUser -match '^S-1-'){$triggerUser}else{([Security.Principal.NTAccount]::new($triggerUser)).Translate([Security.Principal.SecurityIdentifier]).Value}
    if($triggerSid -ne $sid) { throw 'Logon user verification failed.' }
    if($null -eq $daily -or $daily.Type -ne 2 -or -not $daily.Enabled -or $daily.DaysInterval -ne 1 -or $daily.Repetition.Interval -ne 'PT1M' -or $daily.Repetition.Duration -ne 'P1D' -or $daily.Repetition.StopAtDurationEnd) { throw 'Daily recovery trigger verification failed.' }
}

$service=New-Object -ComObject Schedule.Service
$service.Connect()
$folder=$service.GetFolder('\')
$existing=$null
foreach($candidate in $folder.GetTasks(1)) { if($candidate.Name -ieq $taskName) { $existing=$candidate; break } }
$owned=Test-OwnedTask $existing
if($Action -eq 'Status') {
    [pscustomobject]@{task=$taskName;exists=($null -ne $existing);belongs_to_this_install=$owned;enabled=if($owned){[bool]$existing.Enabled}else{$false};install_directory=$installRoot}
    exit 0
}
if($Action -eq 'Disable') {
    # Never remove or change a task belonging to another LightMon location.
    if($owned) { $folder.DeleteTask($taskName,0) }
    Remove-OwnedLegacyRun
    Write-Output 'Autostart for this installation is disabled. Other installations were not changed.'
    exit 0
}
if(-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'LightMon.exe was not found in the installation directory.' }
if($null -ne $existing -and -not $owned) { throw 'Another installation owns the LightMon startup task. Disable startup from that installation first; no existing task was changed.' }
$definition=$service.NewTask(0)
$definition.RegistrationInfo.Author='LightMon'
$definition.RegistrationInfo.Description='Start LightMon for this user at logon and check each minute. Intentional exit pauses recovery until manual launch or next login. Ordinary privileges only.'
$definition.Principal.UserId=$sid
$definition.Principal.LogonType=3
$definition.Principal.RunLevel=0
$settings=$definition.Settings
$settings.Enabled=$true
$settings.AllowDemandStart=$true
$settings.AllowHardTerminate=$false
$settings.StartWhenAvailable=$true
$settings.MultipleInstances=2
$settings.DisallowStartIfOnBatteries=$false
$settings.StopIfGoingOnBatteries=$false
$settings.RunOnlyIfIdle=$false
$settings.RunOnlyIfNetworkAvailable=$false
$settings.WakeToRun=$false
$settings.ExecutionTimeLimit='PT0S'
$settings.RestartInterval='PT1M'
$settings.RestartCount=3
$settings.IdleSettings.StopOnIdleEnd=$false
$settings.IdleSettings.RestartOnIdle=$false
$logon=$definition.Triggers.Create(9)
$logon.Id='LightMonLogon'
$logon.UserId=$sid
$logon.Delay='PT10S'
$logon.Enabled=$true
$daily=$definition.Triggers.Create(2)
$daily.Id='LightMonDailyAssurance'
$daily.StartBoundary=[DateTimeOffset]::Now.AddMinutes(1).ToString('yyyy-MM-ddTHH:mm:sszzz')
$daily.DaysInterval=1
$daily.Enabled=$true
$daily.Repetition.Interval='PT1M'
$daily.Repetition.Duration='P1D'
$daily.Repetition.StopAtDurationEnd=$false
$entry=$definition.Actions.Create(0)
$entry.Path=$exe
$entry.Arguments=$arguments
$entry.WorkingDirectory=$installRoot
$sddl='D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;'+$sid+')'
# Create or update only after the exact existing principal/action checks above.
$null=$folder.RegisterTaskDefinition($taskName,$definition,6,$sid,$null,3,$sddl)
Assert-Installed ($folder.GetTask($taskName))
Remove-OwnedLegacyRun
Write-Output 'Autostart enabled: 10 seconds after login, with a check every minute. No reboot is required.'

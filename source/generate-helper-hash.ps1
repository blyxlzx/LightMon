$ErrorActionPreference='Stop'
$taskBinary=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../LightMonCpu.exe'))
$taskAlgorithm=[Security.Cryptography.SHA256]::Create()
$taskStream=[IO.File]::OpenRead($taskBinary)
try {$taskDigest=[BitConverter]::ToString($taskAlgorithm.ComputeHash($taskStream)).Replace('-','')}
finally {$taskStream.Dispose();$taskAlgorithm.Dispose()}
$taskBytes=for($i=0;$i -lt $taskDigest.Length;$i+=2){'0x'+$taskDigest.Substring($i,2)}
$taskSource='#pragma once'+[Environment]::NewLine+'namespace lightmon { constexpr unsigned char kCpuTemperatureHelperSha256[32] = { '+($taskBytes -join ', ')+' }; }'+[Environment]::NewLine
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'src/cpu_temperature_helper_hash.h'),$taskSource,[Text.UTF8Encoding]::new($false))
[pscustomobject]@{sha256=$taskDigest;bytes=[IO.FileInfo]::new($taskBinary).Length}|ConvertTo-Json

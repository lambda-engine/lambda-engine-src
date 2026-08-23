# =====================================================================================================
#  UE's staged launcher (game\LambdaEngine.exe) is built from BootstrapPackagedGame, which ships with
#  Epic's own icon as resource group 123. Packaging adds our Application.ico as a *second* group (101)
#  but leaves 123 behind, so the exe carries two application icons and shells can pick the wrong one.
#
#  This removes the leftover group so only the project icon remains. Run after packaging.
# =====================================================================================================
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [int]$KeepGroup = 101
)

Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class ResEdit {
  [DllImport("kernel32.dll", CharSet=CharSet.Auto, SetLastError=true)]
  public static extern IntPtr LoadLibraryEx(string f, IntPtr h, uint flags);
  [DllImport("kernel32.dll")] public static extern bool FreeLibrary(IntPtr h);

  public delegate bool EnumResNameProc(IntPtr hModule, IntPtr type, IntPtr name, IntPtr lParam);
  public delegate bool EnumResLangProc(IntPtr hModule, IntPtr type, IntPtr name, ushort lang, IntPtr lParam);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool EnumResourceNames(IntPtr h, IntPtr type, EnumResNameProc cb, IntPtr l);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool EnumResourceLanguages(IntPtr h, IntPtr type, IntPtr name, EnumResLangProc cb, IntPtr l);

  [DllImport("kernel32.dll", CharSet=CharSet.Auto, SetLastError=true)]
  public static extern IntPtr BeginUpdateResource(string file, bool deleteExisting);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool UpdateResource(IntPtr h, IntPtr type, IntPtr name, ushort lang, IntPtr data, uint cb);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool EndUpdateResource(IntPtr h, bool discard);
}
"@

if (-not (Test-Path $Exe)) { Write-Output "[icon] $Exe not found"; exit 1 }

$RT_ICON = [IntPtr]3
$RT_GROUP_ICON = [IntPtr]14

# Find every (group id, language) pair present in the file.
$h = [ResEdit]::LoadLibraryEx($Exe, [IntPtr]::Zero, 0x2)   # LOAD_LIBRARY_AS_DATAFILE
if ($h -eq [IntPtr]::Zero) { Write-Output "[icon] could not open $Exe"; exit 1 }

$found = New-Object System.Collections.ArrayList
$nameCb = [ResEdit+EnumResNameProc]{
    param($m, $t, $n, $l)
    $langCb = [ResEdit+EnumResLangProc]{
        param($m2, $t2, $n2, $lang, $l2)
        [void]$script:found.Add([pscustomobject]@{ Id = [Int64]$n2; Lang = $lang })
        return $true
    }
    [void][ResEdit]::EnumResourceLanguages($m, $t, $n, $langCb, [IntPtr]::Zero)
    return $true
}
$script:found = $found
[void][ResEdit]::EnumResourceNames($h, $RT_GROUP_ICON, $nameCb, [IntPtr]::Zero)
[void][ResEdit]::FreeLibrary($h)

$toDelete = @($found | Where-Object { $_.Id -ne $KeepGroup })
Write-Output ("[icon] groups present: {0}" -f (($found | ForEach-Object { "$($_.Id)(lang $($_.Lang))" }) -join ', '))
if ($toDelete.Count -eq 0) {
    Write-Output "[icon] nothing to remove - only group $KeepGroup present"
    exit 0
}

$u = [ResEdit]::BeginUpdateResource($Exe, $false)
if ($u -eq [IntPtr]::Zero) { Write-Output "[icon] BeginUpdateResource failed (is the exe running?)"; exit 1 }
foreach ($grp in $toDelete) {
    # Passing null data deletes the resource.
    $ok = [ResEdit]::UpdateResource($u, $RT_GROUP_ICON, [IntPtr]$grp.Id, $grp.Lang, [IntPtr]::Zero, 0)
    Write-Output ("[icon] removing stale icon group {0} (lang {1}): {2}" -f $grp.Id, $grp.Lang, $(if($ok){"ok"}else{"FAILED"}))
}
if (-not [ResEdit]::EndUpdateResource($u, $false)) {
    Write-Output "[icon] EndUpdateResource failed"
    exit 1
}
Write-Output "[icon] done - only group $KeepGroup remains"

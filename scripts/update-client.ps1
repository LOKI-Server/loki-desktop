# Embedded in loki-client.exe. The launcher supplies its paths and parent PID.
$ErrorActionPreference = 'Stop'
$filesPath = Join-Path $stage 'files'
$errorPath = Join-Path $stage 'update-error.txt'
$restart = $false
$changes = @()
$pending = @()

try {
    if ([IO.Path]::GetFileName($application) -ne 'loki-client.exe') {
        throw 'Automatic updates require the portable Loki Client executable.'
    }
    if (!$install) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [void][IO.Directory]::CreateDirectory($filesPath)
        $zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $stage 'update.zip'))
        $prefix = "LOKI-Client-$version-windows-x64/"
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $allowed = '^(loki-client\.exe|[a-z0-9_][a-z0-9_.-]*\.dll|README\.md|LICENSE|SKALD-LICENSE)$'
        [long]$total = 0
        try {
            foreach ($entry in $zip.Entries) {
                $path = $entry.FullName.Replace('\', '/')
                if ($path -ceq $prefix -and $entry.Length -eq 0) { continue }
                if (!$path.StartsWith($prefix, [StringComparison]::Ordinal)) {
                    throw 'The update ZIP has an unexpected layout.'
                }
                $name = $path.Substring($prefix.Length)
                if ($name -notmatch $allowed -or !$seen.Add($name)) {
                    throw 'The update ZIP contains an unexpected or duplicate file.'
                }
                $total += $entry.Length
                if ($seen.Count -gt 128 -or $total -gt 268435456) { throw 'The update ZIP is too large to unpack.' }
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $filesPath $name))
            }
        } finally { $zip.Dispose() }
        $exe = Join-Path $filesPath 'loki-client.exe'
        if (!$seen.Contains('loki-client.exe') -or !$seen.Contains('skald_vcs.dll') -or
            (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion -ne $version) {
            throw 'The update ZIP is missing the expected Loki Client build or Skald runtime.'
        }
        exit 0
    }

    $parent = Get-Process -Id $parentId -ErrorAction SilentlyContinue
    if ($parent -and $parent.Path -eq $application -and !$parent.WaitForExit(120000)) {
        throw 'Loki Client did not close. Open the client and try the update again.'
    }
    $restart = $true
    $directory = Split-Path -Parent $application
    $nonce = [Guid]::NewGuid().ToString('N')
    # Replace the executable last and retain backups until relaunch.
    $files = @(Get-ChildItem -LiteralPath $filesPath -File | Sort-Object { $_.Name -eq 'loki-client.exe' })
    foreach ($file in $files) {
        $destination = Join-Path $directory $file.Name
        $existing = Get-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue
        if ($existing -and ($existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint))) {
            throw "Cannot safely replace $($file.Name)."
        }
        $replacement = "$destination.$nonce.new"
        $backup = "$destination.$nonce.bak"
        $pending += $replacement
        [IO.File]::Copy($file.FullName, $replacement, $false)
        if ($existing) { [IO.File]::Replace($replacement, $destination, $backup) }
        else { [IO.File]::Move($replacement, $destination) }
        $changes += @{ path = $destination; backup = $backup; existed = [bool]$existing }
    }
    $updated = Start-Process -FilePath $application -WorkingDirectory $directory -PassThru
    if ($updated.WaitForExit(2000) -and $updated.ExitCode -ne 0) {
        throw 'The updated Loki Client failed to start.'
    }
    foreach ($change in $changes) {
        if ($change.existed) { try { [IO.File]::Delete($change.backup) } catch {} }
    }
    try { [IO.Directory]::Delete($stage, $true) } catch {}
    exit 0
} catch {
    $message = $_.Exception.Message
    if ($install) {
        $rollbackFailed = $false
        for ($index = $changes.Count - 1; $index -ge 0; $index--) {
            $change = $changes[$index]
            try {
                if ($change.existed) {
                    $discard = "$($change.backup).failed"
                    [IO.File]::Replace($change.backup, $change.path, $discard)
                    try { [IO.File]::Delete($discard) } catch {}
                }
                else { [IO.File]::Delete($change.path) }
            } catch { $rollbackFailed = $true }
        }
        foreach ($path in $pending) { try { [IO.File]::Delete($path) } catch {} }
        if ($rollbackFailed) { $message += ' Rollback was incomplete. Remaining backups are in the application folder.' }
        elseif ($restart) { try { Start-Process -FilePath $application } catch {} }
        try {
            Add-Type -AssemblyName System.Windows.Forms
            [void][Windows.Forms.MessageBox]::Show($message, 'Loki Client update failed')
        } catch {}
    }
    [IO.File]::WriteAllText($errorPath, $message)
    exit 1
}

$dirs = @(
    'C:\Android',
    'D:\Android',
    'C:\platform-tools',
    'D:\platform-tools',
    "$env:USERPROFILE\AppData\Local\Android",
    "$env:USERPROFILE\platform-tools",
    "$env:ProgramFiles\platform-tools",
    'E:\Android'
)
foreach ($d in $dirs) {
    if (Test-Path $d) {
        Get-ChildItem -Path $d -Filter adb.exe -Recurse -Depth 3 -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty FullName
    }
}

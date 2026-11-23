[CmdletBinding()]
param(
    [switch] $SkipTests,
    [switch] $Package,
    [switch] $IncludePresentMon
)

$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio 2022 or Build Tools with Desktop development with C++ is required.'
}
$vcvars = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
if (-not $vcvars) { throw 'The Visual Studio 2022 x64 C++ toolchain was not found.' }

Push-Location -LiteralPath $PSScriptRoot
try {
    New-Item -ItemType Directory -Path build, dist -Force | Out-Null
    $flags = '/nologo /std:c++20 /W4 /EHsc /O2 /MT /utf-8 /permissive- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /I src /Fo:build\'
    $systemLibraries = 'd2d1.lib dwrite.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib uiautomationcore.lib iphlpapi.lib pdh.lib gdi32.lib user32.lib comdlg32.lib wbemuuid.lib uuid.lib'
    $providerLibraries = 'windowsapp.lib winhttp.lib advapi32.lib'
    $compile = 'rc /nologo /i src /fo build\pulsebar.res src\pulsebar.rc && cl {0} /Fe:build\PulseBar.exe src\*.cpp build\pulsebar.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF {1} {2}' -f $flags, $systemLibraries, $providerLibraries
    $test = 'cl {0} /Fe:build\pulse_tests.exe tests\model_tests.cpp build\model.obj build\providers.obj build\fps.obj /link {1} && build\pulse_tests.exe' -f $flags, $providerLibraries
    $commands = '"{0}" >nul && {1}' -f $vcvars, $compile
    if (-not $SkipTests) { $commands += ' && ' + $test }
    $launchers = @('PulseBar-Panel.exe', 'PulseBar-Status.exe', 'PulseBar-HideStatus.exe', 'PulseBar-Edit.exe')
    for ($i = 0; $i -lt $launchers.Count; $i++) {
        $commands += ' && cl {0} /DPULSE_LAUNCHER={1} /Fe:build\{2} launcher\main.cpp build\pulsebar.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO shell32.lib user32.lib' -f $flags, $i, $launchers[$i]
    }
    & $env:ComSpec /d /c $commands
    if ($LASTEXITCODE -ne 0) { throw "PulseBar build or tests failed with exit code $LASTEXITCODE." }

    $executables = @('PulseBar.exe') + $launchers
    $artifacts = @()
    foreach ($exe in $executables) {
        Copy-Item -LiteralPath "build/$exe" -Destination "dist/$exe" -Force
        $artifacts += "dist/$exe"
    }
    $vendor = @()
    if ($IncludePresentMon) {
        New-Item -ItemType Directory -Path build/vendor, dist/vendor -Force | Out-Null
        $presentMon = 'build/vendor/PresentMon.exe'
        if (-not (Test-Path -LiteralPath $presentMon)) {
            $url = 'https://github.com/GameTechDev/PresentMon/releases/download/v2.6.0/PresentMon-2.6.0-x64.exe'
            Invoke-WebRequest -Uri $url -OutFile $presentMon
        }
        $expected = 'b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'
        if ((Get-FileHash -LiteralPath $presentMon -Algorithm SHA256).Hash -ne $expected) { throw 'PresentMon checksum mismatch.' }
        Copy-Item -LiteralPath $presentMon -Destination dist/vendor/PresentMon.exe -Force
        Copy-Item -LiteralPath third_party/PresentMon-LICENSE.txt -Destination dist/vendor/PresentMon-LICENSE.txt -Force
        $vendor = @('dist/vendor')
    }
    if ($Package) {
        Compress-Archive -LiteralPath ($artifacts + $vendor + @('README.md')) -DestinationPath 'dist/PulseBar-win-x64.zip' -Force
        $artifacts += 'dist/PulseBar-win-x64.zip'
    }
    $checksumFiles = $artifacts
    if ($IncludePresentMon) { $checksumFiles += 'dist/vendor/PresentMon.exe', 'dist/vendor/PresentMon-LICENSE.txt' }
    $checksumFiles | ForEach-Object {
        $item = Get-FileHash -LiteralPath $_ -Algorithm SHA256
        '{0}  {1}' -f $item.Hash.ToLowerInvariant(), $_.Substring(5)
    } | Set-Content -LiteralPath 'dist/SHA256SUMS.txt' -Encoding ASCII
    Get-Item -LiteralPath $artifacts | Select-Object FullName, Length
}
finally {
    Pop-Location
}

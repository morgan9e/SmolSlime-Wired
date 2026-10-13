param(
    [string]$AndroidSdk = "$env:ProgramFiles/Unity/Hub/Editor/2022.3.22f1/Editor/Data/PlaybackEngines/AndroidPlayer/SDK",
    [string]$JavaHome = "$env:ProgramFiles/Unity/Hub/Editor/2022.3.22f1/Editor/Data/PlaybackEngines/AndroidPlayer/OpenJDK"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'dist/quest-forwarder'
$build = Join-Path $out ([Guid]::NewGuid().ToString('N'))
$bt = Join-Path $AndroidSdk 'build-tools/32.0.0'
$android = Join-Path $AndroidSdk 'platforms/android-32/android.jar'
foreach ($path in @($android, "$JavaHome/bin/javac.exe", "$bt/aapt.exe", "$bt/d8.bat")) {
    if (!(Test-Path -LiteralPath $path)) { throw "Missing build tool: $path" }
}
New-Item -ItemType Directory -Force "$build/gen", "$build/classes", "$build/dex" | Out-Null
function Check-Step { if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" } }
$previousJava = $env:JAVA_HOME
try {
    $env:JAVA_HOME = $JavaHome
    & "$bt/aapt.exe" package -f -m -M "$PSScriptRoot/AndroidManifest.xml" -S "$PSScriptRoot/res" -I $android -J "$build/gen" -F "$build/resources.apk"
    Check-Step
    $sources = @(Get-ChildItem "$PSScriptRoot/src", "$build/gen" -Recurse -Filter '*.java' | ForEach-Object FullName)
    & "$JavaHome/bin/javac.exe" -encoding UTF-8 -source 8 -target 8 -classpath $android -d "$build/classes" @sources
    Check-Step
    & "$JavaHome/bin/jar.exe" cf "$build/classes.jar" -C "$build/classes" .
    Check-Step
    & "$JavaHome/bin/java.exe" -cp "$bt/lib/d8.jar" com.android.tools.r8.D8 --lib $android --min-api 29 --output "$build/dex" "$build/classes.jar"
    Check-Step
    Copy-Item "$build/resources.apk" "$build/unsigned.apk"
    Push-Location "$build/dex"
    try { & "$bt/aapt.exe" add "$build/unsigned.apk" classes.dex; Check-Step } finally { Pop-Location }
    & "$bt/zipalign.exe" -f 4 "$build/unsigned.apk" "$build/aligned.apk"
    Check-Step
    $key = Join-Path $out 'debug.keystore'
    if (!(Test-Path -LiteralPath $key)) {
        & "$JavaHome/bin/keytool.exe" -genkeypair -keystore $key -storepass android -keypass android -alias androiddebugkey -dname 'CN=Quest Slime Debug' -keyalg RSA -keysize 2048 -validity 10000
        Check-Step
    }
    $apk = Join-Path $out 'QuestSlimeForwarder.apk'
    & "$bt/apksigner.bat" sign --ks $key --ks-pass pass:android --key-pass pass:android --out $apk "$build/aligned.apk"
    Check-Step
    & "$bt/apksigner.bat" verify --verbose $apk
    Check-Step
    Write-Output "Built $apk"
} finally { $env:JAVA_HOME = $previousJava }

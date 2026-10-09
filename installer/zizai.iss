; 自在投影 / Zizai Cast installer (Inno Setup 6), Traditional Chinese, English,
; Japanese and Korean (0.7.0). Build:
;   "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer\zizai.iss
; Optional: /DBuildDir=<dir with 自在投影.exe and its DLLs> (default ..\build-app\bin\Release)
; No admin rights needed; installs per user into a folder on the desktop
; (手機投影 in Chinese, Zizai Cast otherwise). The language follows the
; Windows display language (zh-* -> 繁體中文, ja-* -> 日本語, ko-* -> 한국어,
; else English); /LANG=english|chinesetrad|japanese|korean forces one (the
; app passes its own UI language on updates).

; Product name in the installer's VERSIONINFO (ProductName): stays 自在投影 in
; every language, the app's 本機更新 check reads it (app/updater.cpp).
#define AppName "自在投影"
; /DAppVersion=x.y.z overrides it (fake newer installers for updater tests).
#ifndef AppVersion
  #define AppVersion "0.7.6"
#endif
#define AppExe "自在投影.exe"
#ifndef BuildDir
  #define BuildDir "..\build-app\bin\Release"
#endif

[Setup]
; /DTestAppId={{...} builds a test installer with its own uninstall entry
; (silent install tests next to a real installation).
#ifdef TestAppId
AppId={#TestAppId}
#else
AppId={{6B0F4C1E-2D55-4C4E-9E7A-5A1D0C3E7B21}
#endif
AppName={cm:AppName}
AppVersion={#AppVersion}
AppVerName={cm:AppName} {#AppVersion}
AppPublisher={cm:AppName}
AppPublisherURL=https://github.com/victor900106/ZizaiCast
AppSupportURL=https://github.com/victor900106/ZizaiCast/issues
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}
VersionInfoDescription={#AppName} Zizai Cast Setup
PrivilegesRequired=lowest
DefaultDirName={userdesktop}\{cm:DefaultFolder}
DisableDirPage=no
UsePreviousAppDir=yes
DefaultGroupName={cm:AppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
SetupIconFile=..\app\res\app.ico
UninstallDisplayIcon={app}\程式\{#AppExe}
; Keep the install folder tidy: program files and the uninstaller live in 程式\,
; the top level only has shortcuts, 截圖, 錄影, 安裝檔, the readme and the guide.
; The sub-folder names are the same in both languages (the app looks for them).
UninstallFilesDir={app}\程式
UninstallDisplayName={cm:AppName}
OutputDir=Output
; File name pattern relied on by 本機更新 (自在投影-安裝程式-*.exe in 安裝檔).
OutputBaseFilename={#AppName}-安裝程式-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
; Running-copy check: not AppMutex= but [Code] (InitializeSetup /
; InitializeUninstall), so that an update started by the app itself
; (/UPDATE) can wait for the app to quit instead of failing.
CloseApplications=no
; Language: detected from the Windows UI language (exact, then primary
; language: zh-CN / zh-HK also get 繁體中文); anything else gets English,
; the first entry. No language dialog.
ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage
; The bundled zh-TW file predates a few newer (rare) messages; those fall back to English.
MissingMessagesWarning=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesetrad"; MessagesFile: "ChineseTraditional.isl"
; 0.7.0: official Inno Setup translations (compiler:Languages\).
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

[CustomMessages]
chinesetrad.AppName=自在投影
english.AppName=Zizai Cast
chinesetrad.DefaultFolder=手機投影
english.DefaultFolder=Zizai Cast
chinesetrad.AutoStart=開機時自動啟動自在投影（在系統匣背景執行）
english.AutoStart=Start Zizai Cast automatically when Windows starts (in the tray)
chinesetrad.Readme=使用說明
english.Readme=Read Me
chinesetrad.Tutorial=使用教學
english.Tutorial=User Guide
chinesetrad.Licenses=授權資訊
english.Licenses=Licenses
chinesetrad.Uninstall=解除安裝自在投影
english.Uninstall=Uninstall Zizai Cast
chinesetrad.LaunchApp=立即啟動自在投影
english.LaunchApp=Launch Zizai Cast now
japanese.AppName=Zizai Cast
korean.AppName=Zizai Cast
japanese.DefaultFolder=Zizai Cast
korean.DefaultFolder=Zizai Cast
japanese.AutoStart=Windows の起動時に Zizai Cast を自動的に開始する（通知領域で実行）
korean.AutoStart=Windows 시작 시 Zizai Cast 자동 실행(알림 영역에서 실행)
japanese.Readme=Read Me（英語）
korean.Readme=Read Me(영어)
japanese.Tutorial=使い方ガイド
korean.Tutorial=사용 가이드
japanese.Licenses=ライセンス
korean.Licenses=라이선스
japanese.Uninstall=Zizai Cast のアンインストール
korean.Uninstall=Zizai Cast 제거
japanese.LaunchApp=今すぐ Zizai Cast を起動する
korean.LaunchApp=지금 Zizai Cast 실행

[Tasks]
Name: "autostart"; Description: "{cm:AutoStart}"; Flags: unchecked

[Dirs]
; Screenshots and recordings land here; on uninstall each is removed only if
; empty, so the user's pictures and videos are kept.
Name: "{app}\截圖"
Name: "{app}\錄影"
; 安裝檔: new installers dropped here are offered by the app (本機更新, docs/app.md);
; removed on uninstall only if empty.
Name: "{app}\安裝檔"

[InstallDelete]
; Files from earlier builds that installed everything at the top level.
Type: files; Name: "{app}\{#AppExe}"
Type: files; Name: "{app}\*.dll"
Type: files; Name: "{app}\LICENSE.txt"
Type: files; Name: "{app}\unins000.exe"
Type: files; Name: "{app}\unins000.dat"
; Uninstall shortcut name used by early 0.2.0 builds (now 解除安裝自在投影).
Type: files; Name: "{app}\解除安裝.lnk"
; Up to 0.5.3 the AAC decoder was fdk-aac (GPL-incompatible licence); 0.6.0
; uses FFmpeg's libavcodec (LGPL). Remove the old DLL on upgrade.
Type: files; Name: "{app}\程式\fdk-aac.dll"
; The other languages' top-level shortcuts / readme / guide (an update or a
; reinstall in another language). 日本語 / 한국어 use the English readme and
; the "Zizai Cast" shortcut name, like English.
Type: files; Name: "{app}\自在投影.lnk"; Languages: english japanese korean
Type: files; Name: "{app}\解除安裝自在投影.lnk"; Languages: english japanese korean
Type: files; Name: "{app}\授權資訊.lnk"; Languages: english japanese korean
Type: files; Name: "{app}\使用說明.txt"; Languages: english japanese korean
Type: files; Name: "{app}\自在投影教學.html"; Languages: english japanese korean
Type: files; Name: "{app}\Zizai Cast.lnk"; Languages: chinesetrad
Type: files; Name: "{app}\Read Me.txt"; Languages: chinesetrad
Type: files; Name: "{app}\Uninstall Zizai Cast.lnk"; Languages: chinesetrad japanese korean
Type: files; Name: "{app}\Licenses.lnk"; Languages: chinesetrad japanese korean
Type: files; Name: "{app}\ZizaiCast-Guide.html"; Languages: chinesetrad japanese korean
Type: files; Name: "{app}\Zizai Cast のアンインストール.lnk"; Languages: chinesetrad english korean
Type: files; Name: "{app}\ライセンス.lnk"; Languages: chinesetrad english korean
Type: files; Name: "{app}\ZizaiCast-Guide-ja.html"; Languages: chinesetrad english korean
Type: files; Name: "{app}\Zizai Cast 제거.lnk"; Languages: chinesetrad english japanese
Type: files; Name: "{app}\라이선스.lnk"; Languages: chinesetrad english japanese
Type: files; Name: "{app}\ZizaiCast-Guide-ko.html"; Languages: chinesetrad english japanese
#ifndef TestAppId
Type: files; Name: "{autoprograms}\Zizai Cast.lnk"; Languages: chinesetrad
Type: files; Name: "{autoprograms}\Zizai Cast Read Me.lnk"; Languages: chinesetrad japanese korean
Type: files; Name: "{autoprograms}\Zizai Cast User Guide.lnk"; Languages: chinesetrad japanese korean
Type: files; Name: "{autoprograms}\Zizai Cast 使い方ガイド.lnk"; Languages: chinesetrad english korean
Type: files; Name: "{autoprograms}\Zizai Cast 사용 가이드.lnk"; Languages: chinesetrad english japanese
Type: files; Name: "{autoprograms}\Zizai Cast Read Me（英語）.lnk"; Languages: chinesetrad english korean
Type: files; Name: "{autoprograms}\Zizai Cast Read Me(영어).lnk"; Languages: chinesetrad english japanese
Type: files; Name: "{autoprograms}\自在投影.lnk"; Languages: english japanese korean
Type: files; Name: "{autoprograms}\自在投影 使用說明.lnk"; Languages: english japanese korean
Type: files; Name: "{autoprograms}\自在投影 使用教學.lnk"; Languages: english japanese korean
#endif

[Files]
Source: "{#BuildDir}\{#AppExe}"; DestDir: "{app}\程式"; Flags: ignoreversion
; Non-system imports of 自在投影.exe (dumpbin /dependents): OpenSSL, libplist,
; pthreads4w, FFmpeg libavcodec + libavutil (AAC decoder, LGPL build).
Source: "{#BuildDir}\avcodec-63.dll"; DestDir: "{app}\程式"; Flags: ignoreversion
Source: "{#BuildDir}\avutil-61.dll"; DestDir: "{app}\程式"; Flags: ignoreversion
Source: "{#BuildDir}\libcrypto-3-x64.dll"; DestDir: "{app}\程式"; Flags: ignoreversion
Source: "{#BuildDir}\plist-2.0.dll"; DestDir: "{app}\程式"; Flags: ignoreversion
Source: "{#BuildDir}\pthreadVC3.dll"; DestDir: "{app}\程式"; Flags: ignoreversion
; 翻譯 (0.7.0): the offline translation engine (MPL-2.0, static CRT, system DLLs
; only), loaded at run time. The translation models are NOT bundled: the app
; downloads them after asking (%LOCALAPPDATA%\PhoneMirror\models).
Source: "{#BuildDir}\bergamot.dll"; DestDir: "{app}\程式"; Flags: ignoreversion
; 翻譯 OCR (0.7.0): ONNX Runtime 1.30.0 (MIT, official CPU build, unmodified) runs the
; PaddleOCR text recognition models; the models are NOT bundled (downloaded after asking).
Source: "{#BuildDir}\onnxruntime.dll"; DestDir: "{app}\程式"; Flags: ignoreversion
; VC++ runtime, app-local (copied next to the exe by app/CMakeLists.txt)
Source: "{#BuildDir}\msvcp140.dll"; DestDir: "{app}\程式"; Flags: ignoreversion
Source: "{#BuildDir}\msvcp140_1.dll"; DestDir: "{app}\程式"; Flags: ignoreversion
Source: "{#BuildDir}\vcruntime140.dll"; DestDir: "{app}\程式"; Flags: ignoreversion
Source: "{#BuildDir}\vcruntime140_1.dll"; DestDir: "{app}\程式"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}\程式"; DestName: "LICENSE.txt"; Flags: ignoreversion
; Third-party notices (zh-TW / en) and how to get the Corresponding Source:
; 程式\licenses (opened from the app's About window and the 授權資訊 shortcut).
Source: "..\docs\licenses\第三方授權.txt"; DestDir: "{app}\程式\licenses"; Flags: ignoreversion
Source: "..\docs\licenses\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}\程式\licenses"; Flags: ignoreversion
Source: "..\docs\licenses\SOURCE.md"; DestDir: "{app}\程式\licenses"; Flags: ignoreversion
Source: "..\docs\licenses\onnxruntime\*"; DestDir: "{app}\程式\licenses\onnxruntime"; Flags: ignoreversion
; Android (wireless debugging): adb.exe + AdbWinApi/AdbWinUsbApi.dll (NOTICE.txt),
; scrcpy-server (LICENSE-scrcpy.txt) and LICENSE-qrcodegen.txt, copied to
; <bin>\android-tools by app/CMakeLists.txt (android/third_party/fetch_tools.ps1).
Source: "{#BuildDir}\android-tools\*"; DestDir: "{app}\程式\android-tools"; Flags: ignoreversion
; User guide in both languages next to the exe (the app opens the one of its
; UI language, which can be switched any time) ...
Source: "..\docs\tutorial\自在投影教學.html"; DestDir: "{app}\程式"; Flags: ignoreversion
Source: "..\docs\tutorial\ZizaiCast-Guide.html"; DestDir: "{app}\程式"; Flags: ignoreversion
Source: "..\docs\tutorial\ZizaiCast-Guide-ja.html"; DestDir: "{app}\程式"; Flags: ignoreversion
Source: "..\docs\tutorial\ZizaiCast-Guide-ko.html"; DestDir: "{app}\程式"; Flags: ignoreversion
; ... and the setup language's guide + short readme at the top level.
Source: "..\docs\tutorial\自在投影教學.html"; DestDir: "{app}"; Flags: ignoreversion; Languages: chinesetrad
Source: "..\docs\tutorial\使用說明.txt"; DestDir: "{app}"; Flags: ignoreversion; Languages: chinesetrad
Source: "..\docs\tutorial\ZizaiCast-Guide.html"; DestDir: "{app}"; Flags: ignoreversion; Languages: english
Source: "..\docs\tutorial\ZizaiCast-ReadMe.txt"; DestDir: "{app}"; DestName: "Read Me.txt"; Flags: ignoreversion; Languages: english japanese korean
Source: "..\docs\tutorial\ZizaiCast-Guide-ja.html"; DestDir: "{app}"; Flags: ignoreversion; Languages: japanese
Source: "..\docs\tutorial\ZizaiCast-Guide-ko.html"; DestDir: "{app}"; Flags: ignoreversion; Languages: korean

[Icons]
; Test builds (/DTestAppId) skip the Start menu: same names as the real
; installation's, and uninstalling the test copy would remove those.
#ifndef TestAppId
Name: "{autoprograms}\{cm:AppName}"; Filename: "{app}\程式\{#AppExe}"; WorkingDir: "{app}\程式"
Name: "{autoprograms}\{cm:AppName} {cm:Readme}"; Filename: "{app}\使用說明.txt"; Languages: chinesetrad
Name: "{autoprograms}\{cm:AppName} {cm:Tutorial}"; Filename: "{app}\自在投影教學.html"; Languages: chinesetrad
Name: "{autoprograms}\{cm:AppName} {cm:Readme}"; Filename: "{app}\Read Me.txt"; Languages: english
Name: "{autoprograms}\{cm:AppName} {cm:Tutorial}"; Filename: "{app}\ZizaiCast-Guide.html"; Languages: english
Name: "{autoprograms}\{cm:AppName} {cm:Readme}"; Filename: "{app}\Read Me.txt"; Languages: japanese korean
Name: "{autoprograms}\{cm:AppName} {cm:Tutorial}"; Filename: "{app}\ZizaiCast-Guide-ja.html"; Languages: japanese
Name: "{autoprograms}\{cm:AppName} {cm:Tutorial}"; Filename: "{app}\ZizaiCast-Guide-ko.html"; Languages: korean
#endif

Name: "{app}\{cm:AppName}"; Filename: "{app}\程式\{#AppExe}"; WorkingDir: "{app}\程式"
Name: "{app}\{cm:Uninstall}"; Filename: "{uninstallexe}"
Name: "{app}\{cm:Licenses}"; Filename: "{app}\程式\licenses"

[Registry]
; The Run value name 自在投影 is an identifier shared with the app (same in both languages).
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\程式\{#AppExe}"" --background"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\程式\{#AppExe}"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent; Check: not IsUpdate
; 自動更新: the app starts "setup /SILENT /SUPPRESSMSGBOXES /UPDATE [/APPARGS="--dev --background"]"
; and quits; start it again afterwards (also when silent) with the same arguments.
Filename: "{app}\程式\{#AppExe}"; Parameters: "{param:APPARGS|}"; Flags: nowait; Check: IsUpdate

[Code]
// ---- running copy / 自動更新 -------------------------------------------------
// /DNoAppMutex test builds skip the interactive check (install next to the
// owner's running copy); their /UPDATE waits for the --dev copy instead.
#ifdef NoAppMutex
  #define RunningMutex "Local\PhoneMirror.Dev"
#else
  #define RunningMutex "Local\PhoneMirror.SingleInstance"
#endif

function IsUpdate: Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/UPDATE') = 0 then
      Result := True;
end;

function AppRunning: Boolean;
begin
  Result := CheckForMutexes('{#RunningMutex}');
end;

function InitializeSetup: Boolean;
var
  I: Integer;
begin
  Result := True;
  if IsUpdate then begin
    // The app started us and is quitting: give it up to 30 s.
    I := 0;
    while AppRunning and (I < 300) do begin
      Sleep(100);
      I := I + 1;
    end;
    if AppRunning then begin
      Log('/UPDATE: the app is still running after 30 s, giving up');
      Result := False;
    end else
      Sleep(1500);  // let the exiting process unmap the exe
    Exit;
  end;
#ifndef NoAppMutex
  while AppRunning do
    if SuppressibleMsgBox(FmtMessage(SetupMessage(msgSetupAppRunningError), [ExpandConstant('{cm:AppName}')]),
                          mbError, MB_OKCANCEL, IDCANCEL) <> IDOK then begin
      Result := False;
      Exit;
    end;
#endif
end;

function InitializeUninstall: Boolean;
begin
  Result := True;
#ifndef NoAppMutex
  while AppRunning do
    if SuppressibleMsgBox(FmtMessage(SetupMessage(msgUninstallAppRunningError), [ExpandConstant('{cm:AppName}')]),
                          mbError, MB_OKCANCEL, IDCANCEL) <> IDOK then begin
      Result := False;
      Exit;
    end;
#endif
end;

// The app's private adb server (程式\android-tools\adb.exe) normally exits
// with the app; stop a leftover one so its files can be replaced / removed.
// Only that adb.exe: other adb servers (Android Studio …) are left alone.
procedure StopBundledAdb;
var
  Adb: String;
  RC: Integer;
begin
  Adb := ExpandConstant('{app}\程式\android-tools\adb.exe');
  if not FileExists(Adb) then
    Exit;
  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoProfile -NonInteractive -Command "Get-Process adb -ErrorAction SilentlyContinue | ' +
    'Where-Object { $_.Path -eq ''' + Adb + ''' } | Stop-Process -Force"',
    '', SW_HIDE, ewWaitUntilTerminated, RC);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopBundledAdb;
  Result := '';
end;

// The app itself can turn autostart on later (tray menu); remove that entry
// too, but only when it points into this installation.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  V: String;
begin
  if CurUninstallStep = usUninstall then
    StopBundledAdb;
  if CurUninstallStep = usUninstall then
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', '{#AppName}', V) then
      if Pos(Lowercase(ExpandConstant('{app}')), Lowercase(V)) > 0 then
        RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', '{#AppName}');
end;

// Autostart turned on from the app before the files moved into 程式\ still
// points at the old top-level exe: repoint it after an upgrade.
procedure CurStepChanged(CurStep: TSetupStep);
var
  V: String;
begin
  if CurStep = ssPostInstall then
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', '{#AppName}', V) then
      if Pos(Lowercase(ExpandConstant('{app}')), Lowercase(V)) > 0 then
        RegWriteStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', '{#AppName}',
          '"' + ExpandConstant('{app}\程式\{#AppExe}') + '" --background');
end;

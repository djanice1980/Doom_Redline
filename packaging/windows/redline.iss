; REDLINE Windows installer (Inno Setup 6, https://jrsoftware.org/isinfo.php).
;
; Build the game first (see docs/packaging.md):
;   cmake --preset windows-release
;   cmake --build --preset windows-release
;   cmake --install build-win --config Release
; then compile this script (right-click > Compile, or ISCC.exe redline.iss).
; The staged files come from build-win\install\bin: redline.exe plus the DLLs
; vcpkg built (SDL3, vorbis, ogg). No Doom data is shipped: the setup wizard
; asks where the player's own doom.wad / doom2.wad is and writes that path to
; redline.cfg next to the executable. The game reads it at start-up; a blank
; answer just makes the game ask on first launch instead.

#define AppName "REDLINE"
#define AppVersion "0.3.0"
#define AppPublisher "David Janice"
#define StageDir "..\..\build-win\install\bin"

[Setup]
AppId={{7D1C0A6E-5E1B-4B7E-9F0B-REDLINE00001}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\redline.exe
OutputDir=..\..\build-win
OutputBaseFilename=redline-{#AppVersion}-setup
SetupIconFile=redline.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
LicenseFile=..\..\LICENSE

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\redline.exe"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\redline.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\redline.exe"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\redline.cfg"

[Code]
var
  WadPage: TInputFileWizardPage;

function GuessWad(): String;
var
  Candidates: array[0..7] of String;
  I: Integer;
begin
  Candidates[0] := ExpandConstant('{commonpf32}\Steam\steamapps\common\Ultimate Doom\rerelease\DOOM.WAD');
  Candidates[1] := ExpandConstant('{commonpf32}\Steam\steamapps\common\Ultimate Doom\base\DOOM.WAD');
  Candidates[2] := ExpandConstant('{commonpf32}\Steam\steamapps\common\Doom 2\rerelease\DOOM2.WAD');
  Candidates[3] := ExpandConstant('{commonpf32}\Steam\steamapps\common\Doom 2\base\DOOM2.WAD');
  Candidates[4] := ExpandConstant('{commonpf32}\GOG Galaxy\Games\DOOM\DOOM.WAD');
  Candidates[5] := ExpandConstant('{commonpf32}\GOG Galaxy\Games\DOOM + DOOM II\DOOM.WAD');
  Candidates[6] := ExpandConstant('{commonpf32}\GOG Galaxy\Games\DOOM II\DOOM2.WAD');
  Candidates[7] := 'C:\DOOM\DOOM.WAD';
  Result := '';
  for I := 0 to 7 do
    if FileExists(Candidates[I]) then begin
      Result := Candidates[I];
      exit;
    end;
end;

procedure InitializeWizard();
begin
  WadPage := CreateInputFilePage(wpSelectDir,
    'Doom game data',
    'Where is your Doom WAD file?',
    'REDLINE draws its monsters, weapons, sounds and music from your own copy of Doom. ' +
    'Pick DOOM.WAD or DOOM2.WAD from Steam, GOG or the original disc. The file is not ' +
    'copied; the game reads it in place. If EXTRAS.WAD from the Doom + Doom II rerelease ' +
    'sits next to it, the modern and SC-55 soundtracks are available too.' + #13#10#13#10 +
    'Leave this empty to choose the file the first time the game runs.');
  WadPage.Add('Doom WAD file:', 'Doom WAD files (*.wad)|*.wad;*.WAD|All files|*.*', '.wad');
  WadPage.Add('Soundtrack file (EXTRAS.WAD, optional):', 'Doom WAD files (*.wad)|*.wad;*.WAD|All files|*.*', '.wad');
  WadPage.Values[0] := GuessWad();
  if (WadPage.Values[0] <> '') and FileExists(ExtractFilePath(WadPage.Values[0]) + 'EXTRAS.WAD') then
    WadPage.Values[1] := ExtractFilePath(WadPage.Values[0]) + 'EXTRAS.WAD';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (WadPage <> nil) and (CurPageID = WadPage.ID) then begin
    if (Trim(WadPage.Values[0]) <> '') and (not FileExists(WadPage.Values[0])) then begin
      MsgBox('That file does not exist. Pick DOOM.WAD / DOOM2.WAD, or leave the box empty to choose it in the game.', mbError, MB_OK);
      Result := False;
    end else if (Trim(WadPage.Values[1]) <> '') and (not FileExists(WadPage.Values[1])) then begin
      MsgBox('The soundtrack file does not exist. Pick EXTRAS.WAD from the Doom + Doom II rerelease, or leave it empty.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Cfg: String;
begin
  if CurStep = ssPostInstall then begin
    Cfg := ExpandConstant('{app}\redline.cfg');
    if FileExists(Cfg) then DeleteFile(Cfg);
    if Trim(WadPage.Values[0]) <> '' then
      SaveStringToFile(Cfg, 'wad=' + Trim(WadPage.Values[0]) + #13#10, False);
    if Trim(WadPage.Values[1]) <> '' then
      SaveStringToFile(Cfg, 'extras=' + Trim(WadPage.Values[1]) + #13#10, True);
  end;
end;

; Installateur d'Omnis (Inno Setup 6).
;
;   iscc /DStage=<dossier de cmake --install> /DVersion=0.2.0 /DVcRedist=<vc_redist.x64.exe> installer\omnis.iss
;
; Installé pour TON compte seulement (pas besoin d'être administrateur) dans %LOCALAPPDATA%\Programs\Omnis :
; Omnis écrit à côté de lui (modèles, mémoire, réglages), ce qui est interdit dans « Program Files ».
; Les modèles (plusieurs Go) ne sont pas dans l'installateur : au 1er lancement, Omnis mesure le PC et
; télécharge ceux qui lui conviennent (en arrière-plan, avec reprise).

#ifndef Version
  #define Version "0.2.0"
#endif
#ifndef Stage
  #define Stage "..\stage"
#endif
#ifndef CpuTarget
  #define CpuTarget "zen4"
#endif

[Setup]
AppId={{6B0F3C2E-5B7A-4C1E-9E7D-0A11C0FFEE42}
AppName=Omnis
AppVersion={#Version}
AppVerName=Omnis {#Version}
AppPublisher=Omnis
DefaultDirName={localappdata}\Programs\Omnis
DefaultGroupName=Omnis
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir=..\dist
OutputBaseFilename=OmnisSetup-{#Version}-{#CpuTarget}
Compression=lzma2/ultra64
SolidCompression=yes
LZMANumBlockThreads=4
WizardStyle=modern
UninstallDisplayName=Omnis
UninstallDisplayIcon={app}\CompanionAI.exe
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"

[CustomMessages]
fr.TaskDesktop=Créer un raccourci sur le Bureau
fr.TaskStartup=Lancer Omnis au démarrage de Windows
fr.TaskCable=Ouvrir la page de VB-Cable (gratuit) pour qu'Omnis parle dans tes calls Discord
fr.RunOmnis=Lancer Omnis maintenant (il propose de télécharger les modèles adaptés à ton PC)
fr.NoVulkan=Aucun pilote Vulkan trouvé (vulkan-1.dll).%n%nOmnis fonctionnera, mais son cerveau tournera sur le processeur. Installe le dernier pilote AMD Adrenalin pour utiliser ta carte graphique.
fr.RemoveData=Supprimer aussi les modèles téléchargés, la mémoire d'Omnis, ses fiches de voix et ses réglages ?%n%n(Non = tout est gardé pour une réinstallation.)

[Tasks]
Name: "desktop"; Description: "{cm:TaskDesktop}"
Name: "startup"; Description: "{cm:TaskStartup}"; Flags: unchecked
Name: "cable"; Description: "{cm:TaskCable}"; Flags: unchecked

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; Réglages : l'exemple devient companion.json au 1er install, jamais écrasé ensuite.
Source: "{#Stage}\companion.example.json"; DestDir: "{app}"; DestName: "companion.json"; Flags: onlyifdoesntexist uninsneveruninstall
#ifdef VcRedist
Source: "{#VcRedist}"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall; Check: NeedsVcRedist
#endif

[Dirs]
Name: "{app}\assets\models"

[Icons]
Name: "{autoprograms}\Omnis"; Filename: "{app}\CompanionAI.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\Désinstaller Omnis"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Omnis"; Filename: "{app}\CompanionAI.exe"; WorkingDir: "{app}"; Tasks: desktop

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Omnis"; \
  ValueData: """{app}\CompanionAI.exe"""; Flags: uninsdeletevalue; Tasks: startup

[Run]
#ifdef VcRedist
; Runtime Microsoft (utilisé par ONNX Runtime) : seulement s'il manque ; Windows demande l'autorisation.
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Runtime Microsoft Visual C++…"; \
  Flags: waituntilterminated shellexec; Check: NeedsVcRedist
#endif
Filename: "https://vb-audio.com/Cable/"; Flags: shellexec skipifsilent nowait; Tasks: cable
Filename: "{app}\CompanionAI.exe"; WorkingDir: "{app}"; Description: "{cm:RunOmnis}"; Flags: postinstall skipifsilent nowait

[UninstallRun]
Filename: "{cmd}"; Parameters: "/c taskkill /im CompanionAI.exe /f"; Flags: runhidden; RunOnceId: "KillOmnis"

[Code]
function NeedsVcRedist: Boolean;
var
  installed: Cardinal;
begin
  Result := not (RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', installed)
                 and (installed = 1));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and not WizardSilent then
    if not FileExists(ExpandConstant('{sys}\vulkan-1.dll')) then
      MsgBox(CustomMessage('NoVulkan'), mbInformation, MB_OK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  app: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    app := ExpandConstant('{app}');
    if UninstallSilent then exit;
    if MsgBox(CustomMessage('RemoveData'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    begin
      DelTree(app, True, True, True);                                    // modèles, réglages
      DelTree(ExpandConstant('{localappdata}\CompanionAI'), True, True, True);  // mémoire, voix, vannes apprises
    end;
  end;
end;

; Inno Setup 6 script for the SPAStrip Windows installer (VST3 effect, x64 only).
;
; CI (.github/workflows/build.yml) builds it with:
;   ISCC /DAppVersion=<version from CMakeLists.txt> ^
;        /DArtefacts=..\..\build\SPAStrip_artefacts\Release ^
;        /DDocsDir=..\..\build\docs /O<abs dist dir> installers\windows\SPAStrip.iss
; where build\docs is filled by scripts\prepare_docs.sh (README/QUICKSTART with the
; version substituted, EULA, and CREDITS.txt generated from assets/irs/CREDITS.md).
;
; Code signing is opt-in (Windows is unsigned for now). Inno Setup only accepts a
; NAME in the SignTool directive; the command behind that name is defined on the
; ISCC command line with /S. So to sign, pass both:
;   ISCC /DSignToolCmd=spastripsign ^
;        "/Sspastripsign=signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /a $f" ...
; ($f is replaced by Inno with the file to sign.) Inno then signs Setup.exe and
; the uninstaller. This differs from SPASynth's script, which put the whole
; command in /DSignToolCmd; as far as we can tell that is not valid Inno syntax,
; and it was never exercised there. UNVERIFIED until a certificate exists.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef Artefacts
  #define Artefacts "..\..\build\SPAStrip_artefacts\Release"
#endif
#ifndef DocsDir
  #define DocsDir "..\..\build\docs"
#endif

[Setup]
; Unique to SPAStrip. Never change it between versions (it identifies upgrades),
; and never reuse it for another product.
AppId={{26363DA4-D923-4FE6-AAC5-D267BA0DB6BB}
AppName=SPAStrip
AppVersion={#AppVersion}
AppPublisher=Silverplatter Audio
AppPublisherURL=https://www.silverplatteraudio.com
AppSupportURL=https://www.silverplatteraudio.com
DefaultDirName={commonpf64}\Silverplatter Audio\SPAStrip
DefaultGroupName=Silverplatter Audio
DisableProgramGroupPage=yes
PrivilegesRequired=admin
MinVersion=10.0
LicenseFile={#DocsDir}\EULA.txt
OutputBaseFilename=SPAStrip-{#AppVersion}-Windows
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Stamps the version into the installer's PE version resource, which
; scripts/build_release.sh reads to check the downloaded exe is the right build.
VersionInfoVersion={#AppVersion}.0
VersionInfoProductVersion={#AppVersion}.0
VersionInfoCompany=Silverplatter Audio
VersionInfoProductName=SPAStrip
VersionInfoDescription=SPAStrip installer
#ifdef SignToolCmd
SignTool={#SignToolCmd}
SignedUninstaller=yes
#endif

[Files]
; The whole VST3 bundle folder. The uninstaller removes it (and the empty folders).
Source: "{#Artefacts}\VST3\SPAStrip.vst3\*"; DestDir: "{commoncf64}\VST3\SPAStrip.vst3"; \
    Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#DocsDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DocsDir}\QUICKSTART.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DocsDir}\EULA.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DocsDir}\CREDITS.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\SPAStrip README"; Filename: "{app}\README.txt"
Name: "{group}\SPAStrip Credits"; Filename: "{app}\CREDITS.txt"

[Messages]
; Finished page: one newsletter line (plain text; the URL is shown, not clickable).
FinishedLabel=Setup has finished installing [name] on your computer.%n%nStay in the loop: get the Silverplatter Audio newsletter at silverplatteraudio.com/pages/newsletter
FinishedLabelNoIcons=Setup has finished installing [name] on your computer.%n%nStay in the loop: get the Silverplatter Audio newsletter at silverplatteraudio.com/pages/newsletter

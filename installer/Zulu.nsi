; Zulu installer for Command & Conquer: Generals Zero Hour
;
; Build with:  makensis installer/Zulu.nsi
;          or: make installer   (drives this script with staged inputs)
; Output:      installer/Zulu_Setup.exe
;
; Inputs default to ../generalszh_zulu.exe, ../ZuluLauncher.exe and ../Zulu.big
; (relative to this script). The Makefile overrides them with /D to point at
; the staged copies under build/installer-tmp/ so the repo root stays clean.
;
; Layout produced on the target machine:
;   <user-chosen install dir>\generalszh_zulu.exe
;   <user-chosen install dir>\ZuluLauncher.exe
;   <user-chosen install dir>\Uninstall_Zulu.exe
;   %USERPROFILE%\Documents\Command and Conquer Generals Zero Hour Data\Zulu.big
;   %USERPROFILE%\Documents\Command and Conquer Generals Zero Hour Data\ReplayData\
;   Desktop and Start Menu shortcuts that launch:
;     <install dir>\ZuluLauncher.exe -mod Zulu.big
;   The launcher fetches https://storage.googleapis.com/zulu-installer/latest.json
;   on every start, downloads the published installer whenever its version
;   differs from the installed one (either direction, so a rollback is just an
;   older version in latest.json), runs it
;   with /S /D=<install dir>, then exits. The installer's Section calls the
;   launcher again at the end when run silently, so an update completes with
;   one UAC click and no extra shortcut clicks.

!define APPNAME       "Zulu"
!define APPVERSION    "1.6.1"
!define EXENAME       "generalszh_zulu.exe"
!define LAUNCHERNAME  "ZuluLauncher.exe"
!define BIGNAME       "Zulu.big"
!define USERDATALEAF  "Command and Conquer Generals Zero Hour Data"
!define LAUNCHARGS    "-mod Zulu.big"
!define REPLAYDATALEAF "ReplayData"
!define THEATERARGS   "-replaytheater"
!define UNINSTREGKEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"
!define ZHREGKEY      "SOFTWARE\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour"
!define DEFAULTINSTDIR "$PROGRAMFILES\EA Games\Command and Conquer Generals Zero Hour"

; Source paths for the files that get packed into the installer. The
; Makefile passes /D overrides; the defaults preserve the historical
; "binaries sit at the repo root" workflow.
!ifndef EXE_SOURCE
    !define EXE_SOURCE "..\${EXENAME}"
!endif
!ifndef LAUNCHER_SOURCE
    !define LAUNCHER_SOURCE "..\${LAUNCHERNAME}"
!endif
!ifndef BIG_SOURCE
    !define BIG_SOURCE "..\${BIGNAME}"
!endif
!ifndef REPLAYDATA_SOURCE
    !define REPLAYDATA_SOURCE "..\build\installer-tmp\${REPLAYDATALEAF}"
!endif

Name        "${APPNAME}"
OutFile     "Zulu_Setup.exe"
Unicode     true
SetCompressor /SOLID lzma
; The ReplayData archives are ~95% identical to one another and to Zulu.big,
; but LZMA can only dedupe what fits in its window at once, and at the 8 MB
; default they do not all fit. At 64 MB the 16 of them (71 MB on disk) add
; ~470 KB to the installer. Costs compression-time memory only.
SetCompressorDictSize 64

; Default install dir: the location our own uninstaller wrote, so silent
; update-installs from the launcher land in the right place. Failing that
; this literal is only a placeholder -- .onInit replaces it with wherever
; Zero Hour actually is (see DetectZeroHourDir), since the retail path
; below is wrong for every Steam, Origin and EA App owner. The user can
; still change it on the Directory page; for `/S` (silent) installs from
; the launcher the correct path is forwarded via `/D=` anyway.
InstallDirRegKey HKLM "${UNINSTREGKEY}" "InstallLocation"
InstallDir       "${DEFAULTINSTDIR}"

; Writing to Program Files needs admin; the wizard will trigger UAC.
RequestExecutionLevel admin

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define MUI_ABORTWARNING

; First-time interactive install: offer to launch Zulu when the user clicks
; Finish. For silent /S installs (triggered by the launcher's update flow)
; the finish page is skipped entirely; see the Exec at the end of Section
; Install which handles that case.
!define MUI_FINISHPAGE_RUN "$INSTDIR\${LAUNCHERNAME}"
!define MUI_FINISHPAGE_RUN_PARAMETERS "${LAUNCHARGS}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APPNAME}"

!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE DirectoryLeave
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------
; Finding the Zero Hour install
;
; Zulu is a mod, not a standalone game: generalszh_zulu.exe loads Zero Hour's
; own archives (INIZH.big, W3DZH.big, Maps\, Data\) out of the directory it
; runs from, so it has to be installed INTO the user's Zero Hour folder.
;
; Until 1.5.9 the only thing offered was the retail EA Games path under
; Program Files. Steam owners' game is not there, so the wizard proposed a
; folder that did not exist, cheerfully created it, dropped the exe and the
; launcher into it, and produced an install that could not start -- with no
; error until the game was launched.
;
; So: find where Zero Hour actually is, and prove every guess by looking for
; data files only a Zero Hour install has, rather than trusting a registry
; key that may be stale or absent. Sources, in order:
;   1. our own uninstall key (handled by InstallDirRegKey above)
;   2. the Electronic Arts key retail, Origin and the EA App all write
;   3. every Steam library on the machine, from libraryfolders.vdf
;   4. the historical fixed paths
; ---------------------------------------------------------------------------

Var ZHDetectedDir

; $R0 = a directory -> $R0 = "1" when Zero Hour's data files are in it.
;
; INIZH.big sits in the install root on most SKUs and under Data\INI on the
; English/Chinese/Korean discs (Win32BIGFileSystem.cpp skips the duplicate);
; W3DZH.big is present either way. Any one of them settles it, and none of
; them exist in a plain Generals (non-Zero-Hour) folder, so this will not
; match the wrong half of a two-game install.
Function IsZeroHourDir
    Push $R1
    StrCpy $R1 "0"
    ${If} ${FileExists} "$R0\INIZH.big"
    ${OrIf} ${FileExists} "$R0\Data\INI\INIZH.big"
    ${OrIf} ${FileExists} "$R0\W3DZH.big"
        StrCpy $R1 "1"
    ${EndIf}
    StrCpy $R0 $R1
    Pop $R1
FunctionEnd

; $R0 = directory to search, $R1 = how many levels below it to look.
; -> $R0 = "1" and $ZHDetectedDir set to the match, or $R0 = "0".
;
; Depth matters because stores nest differently: a registry key names the
; game folder itself (depth 0 would do, 1 costs nothing and covers a key
; that names the parent), while under steamapps\common the game is one
; folder down, or two when it ships inside a collection folder.
Function ScanForZeroHour
    Push $R2    ; remaining depth
    Push $R3    ; directory being searched
    Push $R4    ; FindFirst handle
    Push $R5    ; directory entry

    StrCpy $R3 $R0
    StrCpy $R2 $R1

    Call IsZeroHourDir
    StrCmp $R0 "1" 0 descend
        StrCpy $ZHDetectedDir $R3
        Goto found

descend:
    IntCmp $R2 0 not_found not_found 0
    FindFirst $R4 $R5 "$R3\*"
    StrCmp $R4 "" not_found

next_entry:
    StrCmp $R5 "" close_not_found
    StrCmp $R5 "." skip_entry
    StrCmp $R5 ".." skip_entry
    StrCpy $R0 "$R3\$R5"
    IntOp $R1 $R2 - 1
    Call ScanForZeroHour
    StrCmp $R0 "1" close_found
skip_entry:
    FindNext $R4 $R5
    Goto next_entry

close_found:
    FindClose $R4
    Goto found
close_not_found:
    FindClose $R4
not_found:
    StrCpy $R0 "0"
    Goto done
found:
    StrCpy $R0 "1"
done:
    Pop $R5
    Pop $R4
    Pop $R3
    Pop $R2
FunctionEnd

; $R0 = a path that came from the registry or a fixed guess. May be empty,
; may have a trailing separator, and may name the game exe instead of the
; folder holding it (the InstallPath value has historically been written
; both ways -- see Core\Tools\Launcher\findpatch.cpp).
; -> $R0 = "1" and $ZHDetectedDir set when it pans out.
Function TryCandidate
    Push $R1
    StrCmp $R0 "" no

    StrCpy $R1 $R0 1 -1
    StrCmp $R1 "\" 0 not_trailing
        StrCpy $R0 $R0 -1
not_trailing:

    StrCpy $R1 $R0 4 -4
    StrCmp $R1 ".exe" 0 not_exe
        ${GetParent} "$R0" $R0
not_exe:

    StrCpy $R1 1
    Call ScanForZeroHour
    Goto done
no:
    StrCpy $R0 "0"
done:
    Pop $R1
FunctionEnd

; $R0 = one line of a libraryfolders.vdf
; -> $R0 = the library root it names, or "" when the line names something
;    else. Both client formats put the path in the line's second quoted
;    token -- new: ["path"]["D:\\SteamLibrary"], old: ["1"]["D:\\SteamLibrary"]
;    -- so rather than matching the key, take any second token that looks
;    like an absolute path. A false positive only costs one wasted scan;
;    the data-file check is what actually decides.
Function GetVdfLibraryPath
    Push $R1    ; scan position
    Push $R2    ; character
    Push $R3    ; first quoted token
    Push $R4    ; second quoted token
    Push $R5    ; tokens completed so far
    Push $R6    ; 1 while inside a quoted token
    Push $R7    ; the line

    StrCpy $R7 $R0
    StrCpy $R1 0
    StrCpy $R3 ""
    StrCpy $R4 ""
    StrCpy $R5 0
    StrCpy $R6 0

scan:
    StrCpy $R2 $R7 1 $R1
    StrCmp $R2 "" scan_done
    IntOp $R1 $R1 + 1
    StrCmp $R2 '"' quote
    StrCmp $R6 "1" 0 scan
    StrCmp $R5 "0" 0 append_second
    StrCpy $R3 "$R3$R2"
    Goto scan
append_second:
    StrCpy $R4 "$R4$R2"
    Goto scan

quote:
    StrCmp $R6 "1" close_token
    StrCpy $R6 1
    Goto scan
close_token:
    StrCpy $R6 0
    IntOp $R5 $R5 + 1
    IntCmp $R5 2 scan_done
    Goto scan

scan_done:
    ; Absolute path test: second character is the drive colon.
    StrCpy $R2 $R4 1 1
    StrCmp $R2 ":" 0 nope
    StrCpy $R0 $R4
    Goto done
nope:
    StrCpy $R0 ""
done:
    Pop $R7
    Pop $R6
    Pop $R5
    Pop $R4
    Pop $R3
    Pop $R2
    Pop $R1
FunctionEnd

; $R0 = path -> $R0 with forward slashes turned into backslashes and runs of
; separators squeezed to one. Steam writes its own location with forward
; slashes under HKCU, and vdf escapes every backslash, so paths arrive as
; "c:/program files (x86)/steam" or "D:\\SteamLibrary". A leading "\\" is
; left alone so a UNC path survives.
Function NormalizeSlashes
    Push $R1    ; scan position
    Push $R2    ; character
    Push $R3    ; previous character
    Push $R4    ; result
    Push $R5    ; input

    StrCpy $R5 $R0
    StrCpy $R1 0
    StrCpy $R3 ""
    StrCpy $R4 ""
loop:
    StrCpy $R2 $R5 1 $R1
    StrCmp $R2 "" done
    IntOp $R1 $R1 + 1
    StrCmp $R2 "/" 0 not_slash
        StrCpy $R2 "\"
not_slash:
    StrCmp $R2 "\" 0 keep
    IntCmp $R1 2 keep keep 0
    StrCmp $R3 "\" loop
keep:
    StrCpy $R4 "$R4$R2"
    StrCpy $R3 $R2
    Goto loop
done:
    StrCpy $R0 $R4
    Pop $R5
    Pop $R4
    Pop $R3
    Pop $R2
    Pop $R1
FunctionEnd

; Walk every Steam library and look for Zero Hour in it.
; -> $R0 = "1" and $ZHDetectedDir set on a hit.
Function TrySteamLibraries
    Push $R1    ; scratch / scan depth
    Push $R6    ; vdf file handle
    Push $R7    ; Steam root
    Push $R8    ; line buffer
    Push $R9    ; library root

    ; Where Steam itself is. The machine-wide key is written with
    ; backslashes; the per-user one uses forward slashes.
    SetRegView 32
    ReadRegStr $R7 HKLM "SOFTWARE\Valve\Steam" "InstallPath"
    StrCmp $R7 "" 0 have_steam
    ReadRegStr $R7 HKCU "Software\Valve\Steam" "SteamPath"
    StrCmp $R7 "" 0 have_steam
    SetRegView 64
    ReadRegStr $R7 HKLM "SOFTWARE\Valve\Steam" "InstallPath"
    SetRegView Default
    StrCmp $R7 "" no_steam

have_steam:
    SetRegView Default
    StrCpy $R0 $R7
    Call NormalizeSlashes
    StrCpy $R7 $R0

    ; The library inside the Steam folder itself. Depth 2: the game is a
    ; folder under common\, or a folder under a collection folder there.
    StrCpy $R0 "$R7\steamapps\common"
    StrCpy $R1 2
    Call ScanForZeroHour
    StrCmp $R0 "1" done

    ; Other drives. Current clients keep the list under steamapps\, clients
    ; older than 2021 under config\.
    ClearErrors
    FileOpen $R6 "$R7\steamapps\libraryfolders.vdf" r
    IfErrors 0 read_line
    ClearErrors
    FileOpen $R6 "$R7\config\libraryfolders.vdf" r
    IfErrors no_steam

read_line:
    ClearErrors
    FileRead $R6 $R8
    IfErrors close_no

    StrCpy $R0 $R8
    Call GetVdfLibraryPath
    StrCmp $R0 "" read_line
    Call NormalizeSlashes
    StrCpy $R9 $R0

    StrCpy $R0 "$R9\steamapps\common"
    StrCpy $R1 2
    Call ScanForZeroHour
    StrCmp $R0 "1" close_yes
    Goto read_line

close_yes:
    FileClose $R6
    Goto done
close_no:
    FileClose $R6
no_steam:
    StrCpy $R0 "0"
done:
    SetRegView Default
    Pop $R9
    Pop $R8
    Pop $R7
    Pop $R6
    Pop $R1
FunctionEnd

; -> $ZHDetectedDir = the Zero Hour folder, or "" when nothing was found.
Function DetectZeroHourDir
    Push $R0
    StrCpy $ZHDetectedDir ""

    ; The key retail, Origin and the EA App write. Read it in both registry
    ; views: we are a 32-bit installer, so plain reads see WOW6432Node only,
    ; and not every store writes there.
    SetRegView 32
    ReadRegStr $R0 HKLM "${ZHREGKEY}" "InstallPath"
    Call TryCandidate
    StrCmp $R0 "1" done
    ReadRegStr $R0 HKCU "${ZHREGKEY}" "InstallPath"
    Call TryCandidate
    StrCmp $R0 "1" done
    SetRegView 64
    ReadRegStr $R0 HKLM "${ZHREGKEY}" "InstallPath"
    SetRegView Default
    Call TryCandidate
    StrCmp $R0 "1" done
    SetRegView 64
    ReadRegStr $R0 HKCU "${ZHREGKEY}" "InstallPath"
    SetRegView Default
    Call TryCandidate
    StrCmp $R0 "1" done

    Call TrySteamLibraries
    StrCmp $R0 "1" done

    ; Fixed paths, for an install whose registry entry never got written.
    StrCpy $R0 "$PROGRAMFILES\EA Games\Command and Conquer Generals Zero Hour"
    Call TryCandidate
    StrCmp $R0 "1" done
    StrCpy $R0 "$PROGRAMFILES64\EA Games\Command and Conquer Generals Zero Hour"
    Call TryCandidate
    StrCmp $R0 "1" done
    StrCpy $R0 "$PROGRAMFILES\Origin Games\Command and Conquer Generals Zero Hour"
    Call TryCandidate
    StrCmp $R0 "1" done
    StrCpy $R0 "$PROGRAMFILES64\Origin Games\Command and Conquer Generals Zero Hour"
    Call TryCandidate

done:
    SetRegView Default
    Pop $R0
FunctionEnd

Function .onInit
    ; $INSTDIR is already filled in by the time we get here: from
    ; InstallDirRegKey when we have installed on this machine before, from
    ; /D= when the launcher's update flow passed one, and otherwise from the
    ; InstallDir literal. Only that last case is a guess, so that is the only
    ; one worth replacing -- an existing install or an explicit /D= knows
    ; better than any search we could run.
    StrCmp $INSTDIR "${DEFAULTINSTDIR}" 0 keep
    Call DetectZeroHourDir
    StrCmp $ZHDetectedDir "" keep
    StrCpy $INSTDIR $ZHDetectedDir
keep:
FunctionEnd

; The directory page's own check, for when detection came up empty or the
; user browses somewhere else. Warn rather than block: a layout we do not
; recognise is possible, and the user knows where their game is.
Function DirectoryLeave
    Push $R0
    StrCpy $R0 $INSTDIR
    Call IsZeroHourDir
    StrCmp $R0 "1" ok
    MessageBox MB_YESNO|MB_ICONEXCLAMATION \
        "$INSTDIR does not look like a Command & Conquer: Generals Zero Hour installation.$\r$\n$\r$\n\
${APPNAME} is a mod. It loads the game's data from the folder it is installed into, so installed anywhere else it will not start.$\r$\n$\r$\n\
Choose the folder holding your Zero Hour files (INIZH.big, Data, Maps). A Steam copy is usually under Steam\steamapps\common, not Program Files.$\r$\n$\r$\n\
Install here anyway?" \
        IDYES ok
    Pop $R0
    Abort
ok:
    Pop $R0
FunctionEnd

Section "Install ${APPNAME}" SecInstall
    SectionIn RO

    ; A silent install never saw the directory page's check, and a /D= from
    ; an old launcher can name a path that is no longer a game folder. Say
    ; so in the log rather than laying files down in silence.
    Push $R0
    StrCpy $R0 $INSTDIR
    Call IsZeroHourDir
    ${If} $R0 != "1"
        DetailPrint "WARNING: $INSTDIR holds no Zero Hour data files (INIZH.big / W3DZH.big)."
        DetailPrint "${APPNAME} loads the game's data from its own folder, so it will not start from here."
    ${EndIf}
    Pop $R0

    ; --- Game executable + launcher -> user-chosen install dir ----------
    ; /oname forces the basename inside the installer regardless of how
    ; the staged input is named on disk.
    SetOutPath "$INSTDIR"
    File "/oname=${EXENAME}" "${EXE_SOURCE}"

    ; The launcher is normally RUNNING while we install: it is what downloaded
    ; and started us, and it waits for us to exit so it can relaunch the game.
    ; Windows keeps an exclusive image lock on a running exe, so writing over
    ; it fails -- and a silent installer swallows that failure and still exits
    ; 0, so the launcher reports a successful update and stays on the old
    ; build. That is why every ZuluLauncher.exe fix since it began updating
    ; itself never reached anyone who updates through the launcher.
    ;
    ; A running exe CAN be renamed, so move it aside and let the write land on
    ; a free name. The stale copy is deleted below when nothing holds it (a
    ; manual install with no launcher running); when the launcher is running it
    ; survives until the Delete at the top of the next update, since the new
    ; launcher's own best-effort sweep cannot delete from Program Files
    ; unelevated. That is one 45 KB file between updates, and
    ; ${LAUNCHERNAME}.old is not an exe extension, so nothing tries to run it.
    Delete "$INSTDIR\${LAUNCHERNAME}.old"
    Rename "$INSTDIR\${LAUNCHERNAME}" "$INSTDIR\${LAUNCHERNAME}.old"
    ; 'try' rather than the default 'on': a write failure must set the error
    ; flag and carry on so the restore below can run, not abort the section.
    SetOverwrite try
    ClearErrors
    File "/oname=${LAUNCHERNAME}" "${LAUNCHER_SOURCE}"
    SetOverwrite on
    IfErrors 0 +2
        ; Could not write the new one: put the old one back rather than leave
        ; the shortcuts pointing at nothing.
        Rename "$INSTDIR\${LAUNCHERNAME}.old" "$INSTDIR\${LAUNCHERNAME}"
    Delete "$INSTDIR\${LAUNCHERNAME}.old"

    ; --- Mod BIG -> user data dir ---------------------------------------
    ; $DOCUMENTS resolves to the invoking user's Documents folder. With UAC
    ; elevation via the consent prompt this is still the original user.
    SetOutPath "$DOCUMENTS\${USERDATALEAF}"
    File "/oname=${BIGNAME}" "${BIG_SOURCE}"

    ; --- Per-release data for old replays -------------------------------
    ; A replay re-runs the simulation, so it needs the data its release
    ; shipped; ours has changed across versions. These are the historical
    ; Zulu.bigs plus the version map that says which one a given replay
    ; needs. Replay Theater mounts the right one via -mod.
    ;
    ; Under the user data dir, never next to the exe. We do not own the
    ; install directory: Generals Online shares it, and GO mounts every .big
    ; it finds underneath it. Our engine fences the folder off
    ; (isInReplayDataFolder in ArchiveFileSystem), GO has no such fence, so a
    ; ReplayData folder there loaded all 16 historical archives into GO and
    ; broke it for anyone running both.
    ;
    ; The price is that the archives are per-account: another Windows account
    ; on the same box gets a Replay Theater with no map until it runs the
    ; installer itself. The launcher names the path it looked in when the map
    ; is missing, so that failure at least explains itself.
    ;
    ; It has to stay in its own subfolder rather than loose in the user data
    ; dir, where the engine's "Zulu*.big" sweep would mount all 16 at once.
    ;
    ; Wiped before extracting so archives dropped from the map (or renamed)
    ; don't accumulate. Everything in there is ours and regenerable.
    RMDir /r "$DOCUMENTS\${USERDATALEAF}\${REPLAYDATALEAF}"
    SetOutPath "$DOCUMENTS\${USERDATALEAF}\${REPLAYDATALEAF}"
    File /r "${REPLAYDATA_SOURCE}\*.*"

    ; Clear the install-dir copy 1.5.6 and 1.5.7 wrote. This is the half of
    ; the fix that matters for people who already have it: until it is gone,
    ; GO keeps picking the archives up from there.
    RMDir /r "$INSTDIR\${REPLAYDATALEAF}"

    ; --- Shortcuts -------------------------------------------------------
    ; Targets the launcher, not the game directly, so every cold start
    ; gets an update check. The launcher forwards the same args to the
    ; game when no update is pending.
    ;
    ; All-users context, so $DESKTOP is the Public desktop and $SMPROGRAMS the
    ; common Start Menu: the exe lives in Program Files, so every account on
    ; the box should see the shortcuts, not just whoever happened to run the
    ; installer. Switched back to the current user afterwards because
    ; $DOCUMENTS above must stay per-user.
    SetShellVarContext all

    ; Drop the per-user copies an older installer left on the installing
    ; account's desktop, otherwise they sit alongside the all-users ones.
    SetShellVarContext current
    Delete "$DESKTOP\${APPNAME}.lnk"
    Delete "$DESKTOP\${APPNAME} Replay Theater.lnk"
    RMDir /r "$SMPROGRAMS\${APPNAME}"
    SetShellVarContext all

    CreateShortcut "$DESKTOP\${APPNAME}.lnk" \
        "$INSTDIR\${LAUNCHERNAME}" \
        "${LAUNCHARGS}" \
        "$INSTDIR\${LAUNCHERNAME}" 0

    ; Replay Theater: same launcher, different mode. It asks which replay to
    ; watch, works out which release's data that replay needs, and starts the
    ; game with that data mounted. Separate shortcut rather than a button in
    ; the game because data is chosen once at startup, before any INI is
    ; parsed, so it cannot be swapped from inside a running game.
    CreateShortcut "$DESKTOP\${APPNAME} Replay Theater.lnk" \
        "$INSTDIR\${LAUNCHERNAME}" \
        "${THEATERARGS}" \
        "$INSTDIR\${LAUNCHERNAME}" 0

    CreateDirectory "$SMPROGRAMS\${APPNAME}"
    CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" \
        "$INSTDIR\${LAUNCHERNAME}" \
        "${LAUNCHARGS}" \
        "$INSTDIR\${LAUNCHERNAME}" 0
    CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME} Replay Theater.lnk" \
        "$INSTDIR\${LAUNCHERNAME}" \
        "${THEATERARGS}" \
        "$INSTDIR\${LAUNCHERNAME}" 0
    CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" \
        "$INSTDIR\Uninstall_Zulu.exe"

    SetShellVarContext current

    ; --- Uninstaller + Add/Remove Programs registration ------------------
    WriteUninstaller "$INSTDIR\Uninstall_Zulu.exe"
    WriteRegStr HKLM "${UNINSTREGKEY}" "DisplayName"     "${APPNAME}"
    WriteRegStr HKLM "${UNINSTREGKEY}" "DisplayVersion"  "${APPVERSION}"
    WriteRegStr HKLM "${UNINSTREGKEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINSTREGKEY}" "UninstallString" '"$INSTDIR\Uninstall_Zulu.exe"'
    WriteRegStr HKLM "${UNINSTREGKEY}" "Publisher"       "Bill Rich"
    WriteRegDWORD HKLM "${UNINSTREGKEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINSTREGKEY}" "NoRepair" 1

    ; --- Windows Firewall allow rule -------------------------------------
    ; The exe is renamed from the retail game, so it inherits no existing
    ; firewall rules; without one, inbound LAN traffic (UDP lobby 8086/8088
    ; and the TCP 8188 observer stream) is silently dropped whenever the
    ; first-listen consent prompt goes unanswered behind the fullscreen
    ; game. We're already elevated, so register the rule here.
    ; Two deletes before the add: the name-based one catches our own rule
    ; from a prior install at a different path; the program-based one wipes
    ; every other inbound rule for this exe, in particular the block rules
    ; Windows auto-creates when the consent prompt is dismissed (block
    ; overrides allow, so a stale one would defeat the rule we add).
    ;
    ; Both deletes are allowed to fail (nothing to delete is the normal first
    ; install), but the add is not: every previous version threw its exit code
    ; away, so a netsh that never ran -- an unelevated install, a policy that
    ; blocks it, a machine where a third-party firewall owns the stack --
    ; produced an install that reported success and silently could not host or
    ; be observed. Verify by asking for the rule back rather than trusting the
    ; add's return code, since that is what actually decides whether inbound
    ; traffic arrives.
    nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="${APPNAME} (${EXENAME})"'
    Pop $0
    nsExec::ExecToLog 'netsh advfirewall firewall delete rule name=all dir=in program="$INSTDIR\${EXENAME}"'
    Pop $0
    nsExec::ExecToLog 'netsh advfirewall firewall add rule name="${APPNAME} (${EXENAME})" dir=in action=allow program="$INSTDIR\${EXENAME}" enable=yes profile=any'
    Pop $0
    DetailPrint "Firewall rule add returned: $0"

    ; show rule exits non-zero when no rule matches the name.
    nsExec::ExecToLog 'netsh advfirewall firewall show rule name="${APPNAME} (${EXENAME})" dir=in'
    Pop $1
    DetailPrint "Firewall rule verification returned: $1"

    ${If} $1 != 0
        DetailPrint "WARNING: no inbound firewall rule for $INSTDIR\${EXENAME}."
        DetailPrint "Hosting a LAN game and being spectated will not work until one exists."
        ; Silent installs come from the launcher's update flow, which runs
        ; unattended: a modal box there would hang the update behind a dialog
        ; nobody is watching. The detail lines above still land in the install
        ; log either way.
        IfSilent +2
        MessageBox MB_OK|MB_ICONEXCLAMATION "Windows Firewall could not be configured for ${APPNAME}.$\r$\n$\r$\nOther players may be unable to join or spectate games you host. To fix this manually, allow inbound connections for:$\r$\n$INSTDIR\${EXENAME}$\r$\n$\r$\nRunning this installer as an administrator usually resolves it."
    ${EndIf}

    ; Silent invocations come from the launcher's update flow. The
    ; *calling* launcher (still running while we install) waits for us
    ; to exit and then re-launches the game with the user's original
    ; argv — so any extras like -zulu_debug carry through. We
    ; intentionally do NOT Exec the launcher here, since that would
    ; only know about the shortcut's hardcoded LAUNCHARGS and would
    ; race the launcher's own relaunch.
SectionEnd

Section "Uninstall"
    Delete "$INSTDIR\${EXENAME}"
    Delete "$INSTDIR\${LAUNCHERNAME}"
    ; The move-aside copy an update left behind if the launcher was still
    ; holding it and never got a chance to sweep it.
    Delete "$INSTDIR\${LAUNCHERNAME}.old"
    Delete "$INSTDIR\Uninstall_Zulu.exe"
    Delete "$DOCUMENTS\${USERDATALEAF}\${BIGNAME}"

    ; Everything in here is ours and regenerable, so clear it wholesale.
    ; RMDir /r is scoped to our own subfolder, never $INSTDIR itself (which
    ; is the user's Zero Hour install) nor the user data dir (their replays,
    ; maps and saves). The $INSTDIR line clears the 1.5.6/1.5.7 location.
    RMDir /r "$DOCUMENTS\${USERDATALEAF}\${REPLAYDATALEAF}"
    RMDir /r "$INSTDIR\${REPLAYDATALEAF}"

    ; Shortcuts are created all-users; clear that context, then sweep the
    ; per-user location too for anything an older installer left behind.
    SetShellVarContext all
    Delete "$DESKTOP\${APPNAME}.lnk"
    Delete "$DESKTOP\${APPNAME} Replay Theater.lnk"
    RMDir /r "$SMPROGRAMS\${APPNAME}"
    SetShellVarContext current
    Delete "$DESKTOP\${APPNAME}.lnk"
    Delete "$DESKTOP\${APPNAME} Replay Theater.lnk"
    RMDir /r "$SMPROGRAMS\${APPNAME}"

    DeleteRegKey HKLM "${UNINSTREGKEY}"

    ; Remove the firewall rule the installer registered.
    nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="${APPNAME} (${EXENAME})"'

    ; Don't RMDir $INSTDIR — that's the user's Zero Hour folder and we
    ; share it with the retail install. Leave it alone.
SectionEnd

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
;   on every start, downloads a new installer if version > installed, runs it
;   with /S /D=<install dir>, then exits. The installer's Section calls the
;   launcher again at the end when run silently, so an update completes with
;   one UAC click and no extra shortcut clicks.

!define APPNAME       "Zulu"
!define APPVERSION    "1.5.7"
!define EXENAME       "generalszh_zulu.exe"
!define LAUNCHERNAME  "ZuluLauncher.exe"
!define BIGNAME       "Zulu.big"
!define USERDATALEAF  "Command and Conquer Generals Zero Hour Data"
!define LAUNCHARGS    "-mod Zulu.big"
!define REPLAYDATALEAF "ReplayData"
!define THEATERARGS   "-replaytheater"
!define UNINSTREGKEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"

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

; Default install dir: prefer the location our own uninstaller wrote (so
; silent update-installs from the launcher land in the right place), then
; the retail EA Games registry key, then a guess. The user can change this
; on the Directory page; for `/S` (silent) installs from the launcher the
; correct path is forwarded via `/D=` anyway.
InstallDirRegKey HKLM "${UNINSTREGKEY}" "InstallLocation"
InstallDir       "$PROGRAMFILES\EA Games\Command and Conquer Generals Zero Hour"

; Writing to Program Files needs admin; the wizard will trigger UAC.
RequestExecutionLevel admin

!include "MUI2.nsh"

!define MUI_ABORTWARNING

; First-time interactive install: offer to launch Zulu when the user clicks
; Finish. For silent /S installs (triggered by the launcher's update flow)
; the finish page is skipped entirely; see the Exec at the end of Section
; Install which handles that case.
!define MUI_FINISHPAGE_RUN "$INSTDIR\${LAUNCHERNAME}"
!define MUI_FINISHPAGE_RUN_PARAMETERS "${LAUNCHARGS}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APPNAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

Section "Install ${APPNAME}" SecInstall
    SectionIn RO

    ; --- Game executable + launcher -> user-chosen install dir ----------
    ; /oname forces the basename inside the installer regardless of how
    ; the staged input is named on disk.
    SetOutPath "$INSTDIR"
    File "/oname=${EXENAME}" "${EXE_SOURCE}"
    File "/oname=${LAUNCHERNAME}" "${LAUNCHER_SOURCE}"

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
    nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="${APPNAME} (${EXENAME})"'
    nsExec::ExecToLog 'netsh advfirewall firewall delete rule name=all dir=in program="$INSTDIR\${EXENAME}"'
    nsExec::ExecToLog 'netsh advfirewall firewall add rule name="${APPNAME} (${EXENAME})" dir=in action=allow program="$INSTDIR\${EXENAME}" enable=yes profile=any'

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

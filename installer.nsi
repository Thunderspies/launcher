Name "Sweet Tea"

InstallDir $PROGRAMFILES\Sweet-Tea
DirText "Choose install directory."

Section

SetOutPath $INSTDIR
File release\Sweet-Tea.exe

createDirectory "$SMPROGRAMS\Thunderspy Gaming"
createShortCut "$SMPROGRAMS\Thunderspy Gaming\Sweet Tea.lnk" "$INSTDIR\Sweet-Tea.exe"

StrCpy $0 "Software\Thunderspy Gaming\Sweet Tea"
StrCpy $1 "manifests"

ReadRegStr $2 HKCU "$0" $1
StrCmp $2 "" newManifests

Goto skipManifests

newManifests:

WriteRegStr HKCU "$0" $1 "${manifests}"

skipManifests:

SectionEnd

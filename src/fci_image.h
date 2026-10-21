/* Image-specific values. The build's recorded image-profile.h may override these. */
#ifndef FCI_IMAGE_H
#define FCI_IMAGE_H
#ifndef FCI_ENTRY_VA
#define FCI_ENTRY_VA 0x004AB7E0u
#endif
#ifndef FCI_XBE_SIZE
#define FCI_XBE_SIZE 6684672u
#endif
#ifndef FCI_IMAGE_FILENAME
#define FCI_IMAGE_FILENAME "default.xbe"
#endif
#ifndef FCI_DESCRIPTOR_JUMP
#define FCI_DESCRIPTOR_JUMP 0x004B03D4u
#endif
#ifndef FCI_CODE_END
#define FCI_CODE_END 0x0070C6E4u
#endif
#ifndef FCI_HAS_SUPPLEMENT
#define FCI_HAS_SUPPLEMENT 0
#endif
/* the title's name in windows and messages, and the installed executables a relaunch switches between */
#ifndef FCI_TITLE_NAME
#define FCI_TITLE_NAME "Far Cry Instincts"
#endif
#ifndef FCI_GAME_EXE_NAME
#define FCI_GAME_EXE_NAME "FarCryInstincts.exe"
#endif
#ifndef FCI_EDITOR_EXE_NAME
#define FCI_EDITOR_EXE_NAME "FarCryInstinctsEditor.exe"
#endif
/* the title's folder under %APPDATA%: profile, logs, shader-cache */
#ifndef FCI_APPDATA
#define FCI_APPDATA "fci-recomp"
#endif
#endif

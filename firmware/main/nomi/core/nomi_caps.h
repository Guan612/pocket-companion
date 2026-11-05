// 这台固件编进了哪些 Nomi 能力（nomi/Kconfig 的 CONFIG_NOMI_*）。
// CONFIG_ 的开关关掉时宏根本不存在，只能在 #if 里用；要在表达式里用（返回给界面、检查应用的权限）就用这里的 0 / 1
#pragma once

#include <sdkconfig.h>

#if CONFIG_NOMI_SDCARD
#define NOMI_HAS_SDCARD 1
#else
#define NOMI_HAS_SDCARD 0
#endif

#if CONFIG_NOMI_APPS
#define NOMI_HAS_APPS 1
#else
#define NOMI_HAS_APPS 0
#endif

#if CONFIG_NOMI_PLAYER
#define NOMI_HAS_PLAYER 1
#else
#define NOMI_HAS_PLAYER 0
#endif

#if CONFIG_NOMI_READER
#define NOMI_HAS_READER 1
#else
#define NOMI_HAS_READER 0
#endif

#if CONFIG_NOMI_PHOTO
#define NOMI_HAS_PHOTO 1
#else
#define NOMI_HAS_PHOTO 0
#endif

#if CONFIG_NOMI_CAMERA
#define NOMI_HAS_CAMERA 1
#else
#define NOMI_HAS_CAMERA 0
#endif

#if CONFIG_NOMI_PHONE
#define NOMI_HAS_PHONE 1
#else
#define NOMI_HAS_PHONE 0
#endif

#if CONFIG_NOMI_HOTSPOT
#define NOMI_HAS_HOTSPOT 1
#else
#define NOMI_HAS_HOTSPOT 0
#endif

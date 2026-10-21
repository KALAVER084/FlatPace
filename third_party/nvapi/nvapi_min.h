// Subconjunto minimo de las declaraciones de NVAPI que usa FlatPace: tipos, codigos de estado,
// identificadores y valores de ajustes del driver, estructuras de perfiles y aplicaciones, tipos de
// las funciones e identificadores para nvapi_QueryInterface.
// Reproducido de los encabezados publicos de NVAPI de NVIDIA (nvapi.h, NvApiDriverSettings.h y
// nvapi_interface.h), con nombres abreviados. Las funciones se obtienen en tiempo de ejecucion de
// nvapi64.dll, que instala el propio driver: FlatPace no incluye ni enlaza la biblioteca de NVAPI.
//
// SPDX-FileCopyrightText: Copyright (c) NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
// Licencia completa: NVAPI-LICENSE.txt, en la raiz del repositorio.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nv {
using NvU32 = uint32_t;
using NvU16 = uint16_t;
using NvU8 = uint8_t;
using NvU64 = uint64_t;
using Status = int;
constexpr int kStringMax = 2048;
using UnicodeString = NvU16[kStringMax];
struct SessionH__ { int unused; };
using Session = SessionH__*;
struct ProfileH__ { int unused; };
using Profile = ProfileH__*;

constexpr Status OK = 0;
constexpr Status SETTING_NOT_FOUND = -160;
constexpr Status PROFILE_NOT_FOUND = -163;
constexpr Status EXECUTABLE_NOT_FOUND = -166;
constexpr Status INVALID_USER_PRIVILEGE = -137;
constexpr Status NVIDIA_DEVICE_NOT_FOUND = -6;
constexpr NvU32 FRL_FPS_ID = 0x10835002;  // "Frame Rate Limiter" (Velocidad maxima de fotogramas)
constexpr NvU32 VSYNCMODE_ID = 0x00A879CF;       // "Vertical Sync"
constexpr NvU32 PRERENDERLIMIT_ID = 0x007BA09E;  // "Maximum pre-rendered frames"
// "Power management mode" (Modo de administracion de energia): 1 = preferir rendimiento maximo.
// Con un limite, el driver baja las frecuencias de la GPU hasta que va justa y tarda en subirlas en
// las escenas pesadas, que se pasan del limite (Greylock con 90 fps: el doble de trabajo por fotograma
// con el 85 % de uso). Es la combinacion que recomienda NVIDIA para su limitador; la GPU sigue
// protegiendose sola por temperatura y potencia.
constexpr NvU32 PREFERRED_PSTATE_ID = 0x1057EB71;
constexpr NvU32 PSTATE_PREFER_MAX = 1;
constexpr NvU32 VSYNC_FORCEON = 0x47814940;
constexpr NvU32 VSYNC_FLIP2 = 0x32610244;        // VSync a 1/2 del refresco
constexpr NvU32 VSYNC_FLIP3 = 0x71271021;        // 1/3
constexpr NvU32 VSYNC_FLIP4 = 0x13245256;        // 1/4
constexpr NvU32 FRL_FPS_MAX = 0x3ff;
constexpr int DWORD_TYPE = 0;
constexpr int CURRENT_PROFILE_LOCATION = 0;

struct BinarySetting {
  NvU32 valueLength;
  NvU8 valueData[4096];
};

#pragma pack(push, 4)
struct Setting {
  NvU32 version;
  UnicodeString settingName;
  NvU32 settingId;
  int settingType;
  int settingLocation;
  NvU32 isCurrentPredefined;
  NvU32 isPredefinedValid;
  union {
    NvU32 u32PredefinedValue;
    BinarySetting binaryPredefinedValue;
    UnicodeString wszPredefinedValue;
    NvU64 u64PredefinedValue;
  };
  union {
    NvU32 u32CurrentValue;
    BinarySetting binaryCurrentValue;
    UnicodeString wszCurrentValue;
    NvU64 u64CurrentValue;
  };
};
#pragma pack(pop)

struct Application {  // NVDRS_APPLICATION_V4
  NvU32 version;
  NvU32 isPredefined;
  UnicodeString appName;
  UnicodeString userFriendlyName;
  UnicodeString launcher;
  UnicodeString fileInFolder;
  NvU32 isMetro : 1;
  NvU32 isCommandLine : 1;
  NvU32 reserved : 30;
  UnicodeString commandLine;
};

struct ProfileInfo {  // NVDRS_PROFILE_V1
  NvU32 version;
  UnicodeString profileName;
  NvU32 gpuSupport;
  NvU32 isPredefined;
  NvU32 numOfApps;
  NvU32 numOfSettings;
};

static_assert(sizeof(Setting) == 12320, "NVDRS_SETTING_V1");
static_assert(sizeof(Application) == 20492, "NVDRS_APPLICATION_V4");
static_assert(sizeof(ProfileInfo) == 4116, "NVDRS_PROFILE_V1");
constexpr NvU32 Ver(size_t size, NvU32 v) { return static_cast<NvU32>(size) | (v << 16); }
constexpr NvU32 SETTING_VER = Ver(sizeof(Setting), 1);
constexpr NvU32 APPLICATION_VER = Ver(sizeof(Application), 4);
constexpr NvU32 PROFILE_VER = Ver(sizeof(ProfileInfo), 1);

using QueryInterfaceFn = void*(__cdecl*)(unsigned int);
using InitializeFn = Status(__cdecl*)();
using CreateSessionFn = Status(__cdecl*)(Session*);
using SessionFn = Status(__cdecl*)(Session);
using FindAppFn = Status(__cdecl*)(Session, NvU16*, Profile*, Application*);
using CreateProfileFn = Status(__cdecl*)(Session, ProfileInfo*, Profile*);
using FindProfileFn = Status(__cdecl*)(Session, NvU16*, Profile*);
using CreateAppFn = Status(__cdecl*)(Session, Profile, Application*);
using GetSettingFn = Status(__cdecl*)(Session, Profile, NvU32, Setting*);
struct VrrInfo {
  NvU32 version;
  NvU32 flags;  // bit 0 activo, 1 posible, 2 pedido, 3 indicador, 4 pantalla en modo VRR
  NvU32 reservedEx[4];
};
using GetDisplayIdFn = Status(__cdecl*)(const char*, NvU32*);
using GetVrrInfoFn = Status(__cdecl*)(NvU32, VrrInfo*);
using SetSettingFn = Status(__cdecl*)(Session, Profile, Setting*);
using DeleteSettingFn = Status(__cdecl*)(Session, Profile, NvU32);
using DeleteProfileFn = Status(__cdecl*)(Session, Profile);

// Identificadores de las funciones, para nvapi_QueryInterface (nvapi_interface.h).
namespace fn {
constexpr unsigned int Initialize = 0x0150e828;
constexpr unsigned int DRS_CreateSession = 0x0694d52e;
constexpr unsigned int DRS_DestroySession = 0xdad9cff8;
constexpr unsigned int DRS_LoadSettings = 0x375dbd6b;
constexpr unsigned int DRS_SaveSettings = 0xfcbc7e14;
constexpr unsigned int DRS_FindApplicationByName = 0xeee566b2;
constexpr unsigned int DRS_CreateProfile = 0xcc176068;
constexpr unsigned int DRS_FindProfileByName = 0x7e4a9a0b;
constexpr unsigned int DRS_CreateApplication = 0x4347a9de;
constexpr unsigned int DRS_GetSetting = 0x73bf8338;
constexpr unsigned int DRS_SetSetting = 0x577dd202;
constexpr unsigned int DRS_DeleteProfileSetting = 0xe4a26362;
constexpr unsigned int DRS_DeleteProfile = 0x17093206;
constexpr unsigned int DISP_GetDisplayIdByDisplayName = 0xae457190;
constexpr unsigned int Disp_GetVRRInfo = 0xdf8fda57;
}  // namespace fn
}  // namespace nv

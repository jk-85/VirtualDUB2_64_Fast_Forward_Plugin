#pragma once

// Minimal ABI declarations extracted from the VirtualDub2 plug-in headers:
//   vd2/plugin/vdplugin.h
//   vd2/plugin/vdtool.h
//
// VirtualDub / VirtualDub2 plug-in header portions are licensed under zlib.
// Copyright (C) 1998-2007 Avery Lee
// Copyright (C) 2015-2019 Anton Shekhovtsov
// Copyright (C) 2024-2025 v0lt

#include <windows.h>
#include <cstddef>
#include <cstdint>

#if defined(_MSC_VER)
using sint64 = signed __int64;
using uint64 = unsigned __int64;
#else
using sint64 = signed long long;
using uint64 = unsigned long long;
#endif
using uint32 = unsigned int;
using int64 = sint64;

struct VDXHWNDStruct;
using VDXHWND = VDXHWNDStruct*;

#ifndef VDXAPIENTRY
#define VDXAPIENTRY __stdcall
#endif

#define VDXMAKEFOURCC(a, b, c, d) \
    ((uint32)(unsigned char)(d) + ((uint32)(unsigned char)(c) << 8) + \
     ((uint32)(unsigned char)(b) << 16) + ((uint32)(unsigned char)(a) << 24))

class IVDXUnknown {
public:
    enum { kIID = VDXMAKEFOURCC('X', 'u', 'n', 'k') };
    virtual int VDXAPIENTRY AddRef() = 0;
    virtual int VDXAPIENTRY Release() = 0;
    virtual void* VDXAPIENTRY AsInterface(uint32 iid) = 0;
};

enum {
    kVDXPlugin_APIVersion = 12
};

enum VDXPluginType {
    kVDXPluginType_Video,
    kVDXPluginType_Audio,
    kVDXPluginType_Input,
    kVDXPluginType_Tool,
    kVDXPluginType_Output,
    kVDXPluginType_AudioEnc
};

using VDXShowStaticAboutProc = bool(VDXAPIENTRY*)(VDXHWND parent);
using VDXShowStaticConfigureProc = bool(VDXAPIENTRY*)(VDXHWND parent);

struct VDXPluginInfo {
    uint32 mSize;
    const wchar_t* mpName;
    const wchar_t* mpAuthor;
    const wchar_t* mpDescription;
    uint32 mVersion;
    uint32 mType;
    uint32 mFlags;
    uint32 mAPIVersionRequired;
    uint32 mAPIVersionUsed;
    uint32 mTypeAPIVersionRequired;
    uint32 mTypeAPIVersionUsed;
    const void* mpTypeSpecificInfo;
    VDXShowStaticAboutProc mpStaticAboutProc;
    VDXShowStaticConfigureProc mpStaticConfigureProc;
};

using VDPluginInfo = VDXPluginInfo;

class IVDXTool : public IVDXUnknown {
public:
    enum { kIID = VDXMAKEFOURCC('X', 'g', 't', '1') };

    virtual bool VDXAPIENTRY GetMenuInfo(int id, char* name, int nameSize, bool* enabled) = 0;
    virtual bool VDXAPIENTRY GetCommandId(int id, char* name, int nameSize) = 0;
    virtual bool VDXAPIENTRY ExecuteMenu(int id, VDXHWND hwndParent) = 0;
    virtual bool VDXAPIENTRY TranslateMessage(MSG& msg) = 0;
    virtual bool VDXAPIENTRY HandleError(const char* text, int source, void* userData) = 0;
    virtual bool VDXAPIENTRY HandleFileOpen(const wchar_t* fileName, const wchar_t* driverName,
                                            VDXHWND hwndParent) = 0;
    virtual void VDXAPIENTRY Attach(VDXHWND hwndParent) = 0;
    virtual void VDXAPIENTRY Detach(VDXHWND hwndParent) = 0;
    virtual bool VDXAPIENTRY HandleFileOpenError(const wchar_t* fileName,
                                                 const wchar_t* driverName,
                                                 VDXHWND hwndParent,
                                                 const char* errorText,
                                                 int source) = 0;
};

class IVDTimeline {
public:
    virtual int64 GetTimelinePos() = 0;
    virtual void GetSelection(int64& start, int64& end) = 0;
    virtual int GetSubsetCount() = 0;
    virtual void GetSubsetRange(int index, int64& start, int64& end) = 0;
    virtual void SetTimelinePos(int64 position) = 0;
};

class IVDToolCallbacks {
public:
    virtual std::size_t GetFileName(wchar_t* buffer, std::size_t count) = 0;
    virtual void SetFileName(const wchar_t* fileName, const wchar_t* driverName,
                             void* userData) = 0;
    virtual void Reopen(void* userData) = 0;
    virtual IVDTimeline* GetTimeline() = 0;
    virtual void Reopen(const wchar_t* fileName, const wchar_t* driverName,
                        void* userData) = 0;
};

struct VDXToolContext {
    uint32 mAPIVersion;
    IVDToolCallbacks* mpCallbacks;
};

using VDXToolCreateProc = bool(VDXAPIENTRY*)(const VDXToolContext* context, IVDXTool** tool);

struct VDXToolDefinition {
    uint32 mSize;
    VDXToolCreateProc mpCreate;
};

enum {
    kVDXPlugin_ToolAPIVersion = 2
};

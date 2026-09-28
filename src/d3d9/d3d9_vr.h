#pragma once

#include <d3d9.h>

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>
#undef VK_USE_PLATFORM_WIN32_KHR

class IDirect3DVR9;
class D3D9DeviceEx;
class SharedTextureHolder;
inline IDirect3DVR9 *g_D3DVR9;

struct D3D9_TEXTURE_VR_DESC {
    uint64_t         Image;
    VkDevice         Device;
    VkPhysicalDevice PhysicalDevice;
    VkInstance       Instance;
    VkQueue          Queue;
    uint32_t         QueueFamilyIndex;

    uint32_t         Width;
    uint32_t         Height;
    VkFormat         Format;
    uint32_t         SampleCount;
};

MIDL_INTERFACE("7e272b32-a49c-46c7-b1a4-ef52936bec87")
IDirect3DVR9 : public IUnknown{
  virtual HRESULT STDMETHODCALLTYPE GetVRDesc(IDirect3DSurface9 * pSurface, D3D9_TEXTURE_VR_DESC * pDesc) = 0;
  virtual HRESULT STDMETHODCALLTYPE TransferSurface(IDirect3DSurface9 *pSurface, BOOL waitResourceIdle) = 0;
  virtual HRESULT STDMETHODCALLTYPE LockDevice() = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockDevice() = 0;
  virtual HRESULT STDMETHODCALLTYPE WaitDeviceIdle() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetBackBufferData(SharedTextureHolder *backBufferData) = 0;
  virtual IDirect3DDevice9 *STDMETHODCALLTYPE GetD3DDevice() = 0;
  // Copy (optionally cropped) backbuffer into a persistent left/right eye RT
  // and fill OpenVR vulkan texture info. eye: 0 = left, 1 = right.
  virtual HRESULT STDMETHODCALLTYPE CaptureEye(int eye, const RECT *srcRect, SharedTextureHolder *outTexture) = 0;
  // Copy the current D3D render target (what the engine just drew) into an eye RT.
  virtual HRESULT STDMETHODCALLTYPE CaptureCurrentRT(int eye, SharedTextureHolder *outTexture) = 0;
  // Copy the backbuffer into a surface that is NEVER submitted as a compositor eye.
  virtual HRESULT STDMETHODCALLTYPE CaptureForOverlay(SharedTextureHolder *outTexture, int cursorX, int cursorY) = 0;
  virtual HRESULT STDMETHODCALLTYPE BlitEyesSBS(SharedTextureHolder *outTexture) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetBlackTexture(SharedTextureHolder *outTexture) = 0;
  virtual HRESULT STDMETHODCALLTYPE BlitEyesToBackbufferSBS() = 0;
  // Vulkan queues may only be touched by one thread at a time. DXVK submits
  // from its own submission thread, so OpenVR's Submit() -- which does a
  // vkQueueSubmit on the SAME queue -- must be bracketed by these or the two
  // race. The symptom is a submission fence that never signals and
  // DxvkDevice::waitForSubmission blocking forever inside Present.
  virtual HRESULT STDMETHODCALLTYPE LockSubmission() = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockSubmission() = 0;
  // Hand the eye render targets back. They are only used in a map, and they
  // are among the largest single allocations in a process that dies of a full
  // 2 GB address space during map loads. Recreated on demand by the next
  // capture. The caller must have stopped submitting them first.
  virtual HRESULT STDMETHODCALLTYPE ReleaseEyeSurfaces() = 0;
  // Diagnostics: snapshot both eye images to disk, in two halves so it never
  // blocks. Issue queues a GPU copy of each eye into system memory and returns
  // at once; Write, called many frames later, reads them back (the copy has
  // long finished, so the wait is already satisfied) and writes BMPs.
  // A synchronous readback here froze the game once already -- do not fold
  // these into one call.
  virtual HRESULT STDMETHODCALLTYPE DiagEyeDumpIssue(UINT maxWidth) = 0;
  virtual HRESULT STDMETHODCALLTYPE DiagEyeDumpWrite(const char *pathPrefix) = 0;
};

#ifdef _MSC_VER
struct __declspec(uuid("7e272b32-a49c-46c7-b1a4-ef52936bec87")) IDirect3DVR9;
#else
__CRT_UUID_DECL(IDirect3DVR9, 0x7e272b32, 0xa49c, 0x46c7, 0xb1, 0xa4, 0xef, 0x52, 0x93, 0x6b, 0xec, 0x87);
#endif

HRESULT __stdcall Direct3DCreateVRImpl(IDirect3DDevice9 *pDevice,
    IDirect3DVR9 **pInterface);
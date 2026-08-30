#include <cstdio>
#include <cmath>
#include "../dxvk/dxvk_include.h"

#include "d3d9_vr.h"

#include "d3d9_include.h"
#include "d3d9_surface.h"

#include "d3d9_device.h"

#include "L4D2VR/game.h"
#include "L4D2VR/vr.h"

namespace dxvk {

// Set from config: draw a VR aiming reticle into each eye.
bool  g_GESVR_DrawReticle = true;
// Force the menu overlay's alpha opaque. See ForceOpaqueAlpha.
bool  g_GESVR_ForceMenuOpaque = true;
// Reticle arm length as a fraction of eye-image height. The original was
// height/90 + 4 (~16px arm at 1080), which reads as very large once the eye
// image is magnified across the headset panel. Tunable via VRReticleSize.
float g_GESVR_ReticleScale = 0.0025f;
// Horizontal correction for the reticle. The eye surface is the full 16:9
// backbuffer, and the whole of it is mapped onto a roughly square eye
// viewport, which squashes X relative to Y -- so a circle drawn in pixels
// comes out as a vertically stretched oval. Widening X by the surface aspect
// cancels that. 0 = derive it from the surface; set a number to override.
float g_GESVR_ReticleAspect = 0.0f;
// 0 = cross, 1 = dot. A dot is far less intrusive at the centre of view.
int   g_GESVR_ReticleStyle = 1;

    class D3D9VR final : public ComObjectClamp<IDirect3DVR9>
    {
    public:

        D3D9VR(IDirect3DDevice9 *pDevice)
            : m_device(static_cast<D3D9DeviceEx *>(pDevice))
        {}

        HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID riid,
            void **ppvObject)
        {
            if (ppvObject == nullptr)
                return E_POINTER;

            *ppvObject = nullptr;

            if (riid == __uuidof(IUnknown) ||
                riid == __uuidof(IDirect3DVR9)) {
                *ppvObject = ref(this);
                return S_OK;
            }

            Logger::warn("D3D9VR::QueryInterface: Unknown interface query");
            Logger::warn(str::format(riid));
            return E_NOINTERFACE;
        }

        HRESULT STDMETHODCALLTYPE GetVRDesc(
            IDirect3DSurface9 *pSurface,
            D3D9_TEXTURE_VR_DESC *pDesc)
        {
            if (unlikely(pSurface == nullptr || pDesc == nullptr))
                return D3DERR_INVALIDCALL;

            D3D9Surface *surface = static_cast<D3D9Surface *>(pSurface);

            const auto *tex = surface->GetCommonTexture();

            const auto &desc = tex->Desc();
            const auto &image = tex->GetImage();
            const auto &device = tex->Device()->GetDXVKDevice();

            // I don't know why the image randomly is a uint64_t in OpenVR.
            pDesc->Image = uint64_t(image->handle());
            pDesc->Device = device->handle();
            pDesc->PhysicalDevice = device->adapter()->handle();
            pDesc->Instance = device->instance()->handle();
            pDesc->Queue = device->queues().graphics.queueHandle;
            pDesc->QueueFamilyIndex = device->queues().graphics.queueIndex;

            pDesc->Width = desc->Width;
            pDesc->Height = desc->Height;
            pDesc->Format = tex->GetFormatMapping().FormatColor;
            pDesc->SampleCount = uint32_t(image->info().sampleCount);

            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE TransferSurface(
            IDirect3DSurface9 *pSurface,
            BOOL waitResourceIdle)
        {
            if (unlikely(pSurface == nullptr))
                return D3DERR_INVALIDCALL;

            auto *tex = static_cast<D3D9Surface *>(pSurface)->GetCommonTexture();
            const auto &image = tex->GetImage();

            VkImageSubresourceRange subresources = {
              VK_IMAGE_ASPECT_COLOR_BIT,
              0, image->info().mipLevels,
              0, image->info().numLayers
            };

            m_device->TransformImage(
                tex, &subresources,
                image->info().layout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

            // OpenVR only needs the copy SUBMITTED to the queue before it reads
            // the VkImage -- it does its own synchronization on the compositor
            // side. WaitForResource here blocks the CPU until the GPU goes idle,
            // and this function runs four times per in-map frame (left eye,
            // right eye, SBS blit, black texture). That was four full pipeline
            // stalls every frame, and the main reason performance was "terrible".
            // Flush submits the work without parking the CPU on a fence.
            if (waitResourceIdle)
                m_device->Flush();

            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE LockDevice()
        {
            m_lock = m_device->LockDevice();
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE UnlockDevice()
        {
            m_lock = D3D9DeviceLock();
            return D3D_OK;
        }

        // Bracket external (OpenVR) queue submissions. DxvkDevice documents
        // this as the mechanism external libraries must use, because Vulkan
        // queues are single-threaded-access only. Without it, OpenVR's
        // vkQueueSubmit races DXVK's submission thread and Present hangs in
        // DxvkDevice::waitForSubmission.
        HRESULT STDMETHODCALLTYPE LockSubmission()
        {
            // Queue mutex only. lockSubmission() also drains pending submissions,
            // which deadlocks when called from inside Present -- the presentation
            // in flight cannot retire while Present waits on it.
            m_device->GetDXVKDevice()->lockSubmissionQueueOnly();
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE UnlockSubmission()
        {
            m_device->GetDXVKDevice()->unlockSubmissionQueueOnly();
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE WaitDeviceIdle()
        {
            m_device->Flush();
            // Not clear if we need all here, perhaps...
            m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
            m_device->GetDXVKDevice()->waitForIdle();
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE GetBackBufferData(SharedTextureHolder* backBufferData)
        {
            IDirect3DSurface9 *backBufferSurface = nullptr;

            HRESULT res = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBufferSurface);
            if (FAILED(res) || !backBufferSurface)
                return res;

            D3D9_TEXTURE_VR_DESC textureDesc;
            GetVRDesc(backBufferSurface, &textureDesc);
            backBufferSurface->Release();

            memcpy(&backBufferData->m_VulkanData, &textureDesc, sizeof(vr::VRVulkanTextureData_t));
            backBufferData->m_VRTexture.handle = &backBufferData->m_VulkanData;
            backBufferData->m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
            backBufferData->m_VRTexture.eType = vr::TextureType_Vulkan;

            return D3D_OK;
        }

        IDirect3DDevice9 *STDMETHODCALLTYPE GetD3DDevice()
        {
            return m_device;
        }

        HRESULT FillEyeFromSurface(int eye, IDirect3DSurface9 *src, const RECT *srcRect, SharedTextureHolder *outTexture)
        {
            if (!outTexture || !src)
                return D3DERR_INVALIDCALL;

            HRESULT hr = EnsureStereoSurfaces();
            if (FAILED(hr))
                return hr;

            IDirect3DSurface9 *dest = (eye == 0) ? m_left : m_right;
            if (src != dest)
            {
                hr = m_device->StretchRect(src, srcRect, dest, nullptr, D3DTEXF_LINEAR);
                if (FAILED(hr))
                {
                    // Most likely cause is a multisampled source: D3D9 resolves
                    // MSAA through StretchRect, but some paths reject a partial
                    // source rect. Say so once instead of silently going black,
                    // so mat_antialias can be ruled in or out from the log.
                    static bool s_logged = false;
                    if (!s_logged) {
                        s_logged = true;
                        if (FILE *f = fopen("C:/Users/Matty/AppData/Local/Temp/gesvr_boot.log", "a")) {
                            fprintf(f, "[EYE] StretchRect FAILED hr=0x%08lX (try mat_antialias 0)\n", (unsigned long)hr);
                            fclose(f);
                        }
                    }
                    return hr;
                }
            }

            // Aiming reticle, drawn straight into the eye image.
            //
            // The game's own crosshair never reaches the headset: +crosshair 1 is
            // set, but the 2D HUD is not part of what we capture. In head-aim mode
            // you aim with the centre of your view, so a centre reticle is both
            // correct and independent of the game's HUD entirely.
            if (g_GESVR_DrawReticle)
            {
                D3DSURFACE_DESC dd{};
                if (SUCCEEDED(dest->GetDesc(&dd)) && dd.Width > 32 && dd.Height > 32)
                {
                    const LONG cx = (LONG)(dd.Width / 2);
                    const LONG cy = (LONG)(dd.Height / 2);
                    float sc = g_GESVR_ReticleScale;
                    if (sc < 0.001f) sc = 0.001f;
                    if (sc > 0.200f) sc = 0.200f;
                    LONG arm = (LONG)(dd.Height * sc);
                    if (arm < 3) arm = 3;
                    LONG th = arm / 5;
                    if (th < 1) th = 1;
                    const D3DCOLOR col = D3DCOLOR_ARGB(255, 255, 245, 120);
                    float ar = g_GESVR_ReticleAspect;
                    if (ar <= 0.0f)
                        ar = (dd.Height > 0) ? ((float)dd.Width / (float)dd.Height) : 1.0f;
                    if (ar < 0.25f) ar = 0.25f;
                    if (ar > 4.0f)  ar = 4.0f;

                    if (g_GESVR_ReticleStyle == 1)
                    {
                        // ColorFill only draws rectangles, so build the dot from one
                        // fill per scanline. The horizontal radius is widened by the
                        // aspect so it lands on screen as a circle, not an oval.
                        const float ry = (float)arm;
                        const float rx = ry * ar;
                        const LONG iry = (LONG)(ry + 0.5f);
                        for (LONG dy = -iry; dy <= iry; ++dy)
                        {
                            const float ny = (float)dy / (ry + 0.5f);
                            const float inside = 1.0f - ny * ny;
                            if (inside <= 0.0f)
                                continue;
                            const LONG hw = (LONG)(rx * sqrtf(inside) + 0.5f);
                            if (hw < 1)
                                continue;
                            RECT row = { cx - hw, cy + dy, cx + hw, cy + dy + 1 };
                            if (row.left < 0) row.left = 0;
                            if (row.top  < 0) row.top  = 0;
                            if (row.right  > (LONG)dd.Width)  row.right  = (LONG)dd.Width;
                            if (row.bottom > (LONG)dd.Height) row.bottom = (LONG)dd.Height;
                            if (row.right > row.left && row.bottom > row.top)
                                m_device->ColorFill(dest, &row, col);
                        }
                    }
                    else
                    {
                        const LONG armX = (LONG)((float)arm * ar);
                        const LONG thX  = (LONG)((float)th * ar) > 0 ? (LONG)((float)th * ar) : 1;
                        RECT hr2 = { cx - armX, cy - th, cx + armX, cy + th };
                        RECT vr2 = { cx - thX, cy - arm, cx + thX, cy + arm };
                        m_device->ColorFill(dest, &hr2, col);
                        m_device->ColorFill(dest, &vr2, col);
                    }
                }
            }

            TransferSurface(dest, TRUE);

            D3D9_TEXTURE_VR_DESC textureDesc{};
            GetVRDesc(dest, &textureDesc);
            memcpy(&outTexture->m_VulkanData, &textureDesc, sizeof(vr::VRVulkanTextureData_t));
            outTexture->m_VRTexture.handle = &outTexture->m_VulkanData;
            outTexture->m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
            outTexture->m_VRTexture.eType = vr::TextureType_Vulkan;
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE CaptureEye(int eye, const RECT *srcRect, SharedTextureHolder *outTexture)
        {
            IDirect3DSurface9 *bb = nullptr;
            HRESULT hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
            if (FAILED(hr) || !bb)
                return hr;
            hr = FillEyeFromSurface(eye, bb, srcRect, outTexture);
            bb->Release();
            return hr;
        }

        HRESULT STDMETHODCALLTYPE CaptureCurrentRT(int eye, SharedTextureHolder *outTexture)
        {
            IDirect3DSurface9 *src = nullptr;
            HRESULT hr = m_device->GetRenderTarget(0, &src);
            if (FAILED(hr) || !src)
            {
                if (src)
                    src->Release();
                return CaptureEye(eye, nullptr, outTexture);
            }

            hr = FillEyeFromSurface(eye, src, nullptr, outTexture);
            src->Release();
            return hr;
        }

        // Force the overlay texture opaque.
        //
        // The overlay RT inherits the backbuffer's format and StretchRect copies
        // the alpha channel verbatim. In a map Source leaves real alpha in there
        // -- VGUI panels write it, the world largely does not -- and OpenVR
        // honours it, so the panel renders semi-transparent with the game showing
        // through. This SDK has no VROverlayFlags_IgnoreTextureAlpha, so the
        // alpha has to be written for real.
        //
        // Writes ONLY the alpha channel via the colour-write mask, so the colour
        // copied above is untouched. All state is saved and restored around it:
        // this runs inside PresentEx while the game is mid-frame.
        void ForceOpaqueAlpha()
        {
            if (!g_GESVR_ForceMenuOpaque || m_alphaSBFailed || !m_overlay)
                return;

            IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr;
            if (FAILED(m_device->GetRenderTarget(0, &oldRT)))
                return;
            m_device->GetDepthStencilSurface(&oldDS);   // may legitimately be null

            if (!m_alphaSB)
            {
                if (FAILED(m_device->CreateStateBlock(D3DSBT_ALL, &m_alphaSB)) || !m_alphaSB)
                {
                    m_alphaSBFailed = true;
                    Game::logMsg("ForceOpaqueAlpha: CreateStateBlock failed, menu alpha left as-is");
                    if (oldRT) oldRT->Release();
                    if (oldDS) oldDS->Release();
                    return;
                }
            }
            else
                m_alphaSB->Capture();

            struct V { float x, y, z, rhw; D3DCOLOR c; };
            const float w = (float)m_overlayW, h = (float)m_overlayH;
            const D3DCOLOR opaque = D3DCOLOR_ARGB(255, 0, 0, 0);
            V quad[4] = {
                { -0.5f,     -0.5f,     0.0f, 1.0f, opaque },
                {  w - 0.5f, -0.5f,     0.0f, 1.0f, opaque },
                { -0.5f,      h - 0.5f, 0.0f, 1.0f, opaque },
                {  w - 0.5f,  h - 0.5f, 0.0f, 1.0f, opaque },
            };

            m_device->SetDepthStencilSurface(nullptr);
            m_device->SetVertexShader(nullptr);
            m_device->SetPixelShader(nullptr);
            m_device->SetTexture(0, nullptr);
            m_device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            m_device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALPHA);
            m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            m_device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
            m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            m_device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
            m_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
            m_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            m_device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            const HRESULT hrRT   = m_device->SetRenderTarget(0, m_overlay);
            const HRESULT hrDraw = m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
            // The alpha write used to fail silently. Say once whether it worked,
            // so a see-through menu can be confirmed or ruled out from the log.
            {
                static bool s_reported = false;
                if (!s_reported)
                {
                    s_reported = true;
                    Game::logMsg("ForceOpaqueAlpha: setRT=0x%08X draw=0x%08X %ux%u",
                                 (unsigned)hrRT, (unsigned)hrDraw, m_overlayW, m_overlayH);
                }
            }

            m_alphaSB->Apply();
            m_device->SetRenderTarget(0, oldRT);
            m_device->SetDepthStencilSurface(oldDS);
            if (oldRT) oldRT->Release();
            if (oldDS) oldDS->Release();
        }

        HRESULT STDMETHODCALLTYPE CaptureForOverlay(SharedTextureHolder *outTexture, int cursorX, int cursorY)
        {
            if (!outTexture)
                return D3DERR_INVALIDCALL;

            IDirect3DSurface9 *bb = nullptr;
            HRESULT hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
            if (FAILED(hr) || !bb)
                return hr;

            D3DSURFACE_DESC desc{};
            bb->GetDesc(&desc);

            if (!m_overlay || m_overlayW != desc.Width || m_overlayH != desc.Height)
            {
                if (m_overlay) { m_overlay->Release(); m_overlay = nullptr; }
                hr = m_device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                    D3DMULTISAMPLE_NONE, 0, FALSE, &m_overlay, nullptr);
                if (FAILED(hr))
                {
                    hr = m_device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_A8R8G8B8,
                        D3DMULTISAMPLE_NONE, 0, FALSE, &m_overlay, nullptr);
                }
                if (FAILED(hr) || !m_overlay)
                {
                    bb->Release();
                    Game::logMsg("CreateRenderTarget overlay failed hr=0x%08X", (unsigned)hr);
                    return hr;
                }
                m_overlayW = desc.Width;
                m_overlayH = desc.Height;
                Game::logMsg("Overlay D3D RT created %ux%u (separate from eye RTs)", m_overlayW, m_overlayH);
            }

            hr = m_device->StretchRect(bb, nullptr, m_overlay, nullptr, D3DTEXF_LINEAR);
            bb->Release();
            if (FAILED(hr))
                return hr;

            if (cursorX >= 0 && cursorY >= 0)
            {
                auto mark = [&](int x0, int y0, int x1, int y1, D3DCOLOR c) {
                    RECT r = { x0, y0, x1, y1 };
                    if (r.left < 0) r.left = 0;
                    if (r.top < 0) r.top = 0;
                    if (r.right > (LONG)m_overlayW) r.right = (LONG)m_overlayW;
                    if (r.bottom > (LONG)m_overlayH) r.bottom = (LONG)m_overlayH;
                    if (r.right > r.left && r.bottom > r.top)
                        m_device->ColorFill(m_overlay, &r, c);
                };
                mark(cursorX - 14, cursorY - 2, cursorX + 14, cursorY + 2, D3DCOLOR_ARGB(255, 255, 230, 40));
                mark(cursorX - 2, cursorY - 14, cursorX + 2, cursorY + 14, D3DCOLOR_ARGB(255, 255, 230, 40));
                mark(cursorX - 4, cursorY - 4, cursorX + 4, cursorY + 4, D3DCOLOR_ARGB(255, 255, 80, 40));
            }

            ForceOpaqueAlpha();

            // No Flush: this runs inside PresentEx, before the swap. Flushing
            // here deadlocks DXVK against the Present that follows. The swap
            // submits the queued layout transition; overlay may lag one frame.
            TransferSurface(m_overlay, FALSE);
            D3D9_TEXTURE_VR_DESC textureDesc{};
            GetVRDesc(m_overlay, &textureDesc);
            memcpy(&outTexture->m_VulkanData, &textureDesc, sizeof(vr::VRVulkanTextureData_t));
            outTexture->m_VRTexture.handle = &outTexture->m_VulkanData;
            // Gamma keeps the GE:S menu from looking washed-out / dim in the HMD.
            outTexture->m_VRTexture.eColorSpace = vr::ColorSpace_Gamma;
            outTexture->m_VRTexture.eType = vr::TextureType_Vulkan;
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE BlitEyesSBS(SharedTextureHolder *outTexture)
        {
            if (!outTexture || !m_left || !m_right)
                return D3DERR_INVALIDCALL;

            const UINT w = m_eyeW * 2;
            const UINT h = m_eyeH;
            if (!m_sbs || m_sbsW != w || m_sbsH != h)
            {
                if (m_sbs) { m_sbs->Release(); m_sbs = nullptr; }
                HRESULT hr = m_device->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8,
                    D3DMULTISAMPLE_NONE, 0, FALSE, &m_sbs, nullptr);
                if (FAILED(hr) || !m_sbs)
                {
                    Game::logMsg("CreateRenderTarget SBS failed hr=0x%08X", (unsigned)hr);
                    return hr;
                }
                m_sbsW = w;
                m_sbsH = h;
                Game::logMsg("SBS overlay RT created %ux%u", w, h);
            }

            RECT leftDest  = { 0, 0, (LONG)m_eyeW, (LONG)m_eyeH };
            RECT rightDest = { (LONG)m_eyeW, 0, (LONG)w, (LONG)h };
            m_device->StretchRect(m_left, nullptr, m_sbs, &leftDest, D3DTEXF_LINEAR);
            m_device->StretchRect(m_right, nullptr, m_sbs, &rightDest, D3DTEXF_LINEAR);
            TransferSurface(m_sbs, TRUE);

            D3D9_TEXTURE_VR_DESC textureDesc{};
            GetVRDesc(m_sbs, &textureDesc);
            memcpy(&outTexture->m_VulkanData, &textureDesc, sizeof(vr::VRVulkanTextureData_t));
            outTexture->m_VRTexture.handle = &outTexture->m_VulkanData;
            outTexture->m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
            outTexture->m_VRTexture.eType = vr::TextureType_Vulkan;
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE GetBlackTexture(SharedTextureHolder *outTexture)
        {
            if (!outTexture)
                return D3DERR_INVALIDCALL;

            if (!m_black)
            {
                HRESULT hr = m_device->CreateRenderTarget(1280, 720, D3DFMT_A8R8G8B8,
                    D3DMULTISAMPLE_NONE, 0, FALSE, &m_black, nullptr);
                if (FAILED(hr) || !m_black)
                    return hr;
                m_device->ColorFill(m_black, nullptr, D3DCOLOR_ARGB(255, 8, 10, 18));
            }

            // Black pixels never change. Transfer+Flush once, then reuse.
            static bool s_blackReady = false;
            static D3D9_TEXTURE_VR_DESC s_blackDesc{};
            if (!s_blackReady)
            {
                TransferSurface(m_black, TRUE);
                GetVRDesc(m_black, &s_blackDesc);
                s_blackReady = true;
            }
            memcpy(&outTexture->m_VulkanData, &s_blackDesc, sizeof(vr::VRVulkanTextureData_t));
            outTexture->m_VRTexture.handle = &outTexture->m_VulkanData;
            outTexture->m_VRTexture.eColorSpace = vr::ColorSpace_Gamma;
            outTexture->m_VRTexture.eType = vr::TextureType_Vulkan;
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE BlitEyesToBackbufferSBS()
        {
            if (!m_left || !m_right)
                return D3DERR_INVALIDCALL;

            IDirect3DSurface9 *bb = nullptr;
            HRESULT hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
            if (FAILED(hr) || !bb)
                return hr;

            D3DSURFACE_DESC desc{};
            bb->GetDesc(&desc);
            RECT leftDest  = { 0, 0, (LONG)desc.Width / 2, (LONG)desc.Height };
            RECT rightDest = { (LONG)desc.Width / 2, 0, (LONG)desc.Width, (LONG)desc.Height };
            m_device->StretchRect(m_left, nullptr, bb, &leftDest, D3DTEXF_LINEAR);
            m_device->StretchRect(m_right, nullptr, bb, &rightDest, D3DTEXF_LINEAR);
            bb->Release();
            return D3D_OK;
        }

    private:
        HRESULT EnsureStereoSurfaces()
        {
            IDirect3DSurface9 *bb = nullptr;
            HRESULT hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
            if (FAILED(hr) || !bb)
                return hr;

            D3DSURFACE_DESC desc{};
            bb->GetDesc(&desc);
            bb->Release();

            if (m_left && m_right && m_eyeW == desc.Width && m_eyeH == desc.Height)
                return D3D_OK;

            if (m_left) { m_left->Release(); m_left = nullptr; }
            if (m_right) { m_right->Release(); m_right = nullptr; }

            D3DFORMAT fmt = desc.Format;
            hr = m_device->CreateRenderTarget(desc.Width, desc.Height, fmt,
                D3DMULTISAMPLE_NONE, 0, FALSE, &m_left, nullptr);
            if (FAILED(hr))
            {
                hr = m_device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_A8R8G8B8,
                    D3DMULTISAMPLE_NONE, 0, FALSE, &m_left, nullptr);
            }
            if (FAILED(hr))
            {
                Game::logMsg("CreateRenderTarget left eye failed hr=0x%08X fmt=%d", (unsigned)hr, (int)fmt);
                return hr;
            }

            hr = m_device->CreateRenderTarget(desc.Width, desc.Height, fmt,
                D3DMULTISAMPLE_NONE, 0, FALSE, &m_right, nullptr);
            if (FAILED(hr))
            {
                hr = m_device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_A8R8G8B8,
                    D3DMULTISAMPLE_NONE, 0, FALSE, &m_right, nullptr);
            }
            if (FAILED(hr))
            {
                Game::logMsg("CreateRenderTarget right eye failed hr=0x%08X", (unsigned)hr);
                m_left->Release();
                m_left = nullptr;
                return hr;
            }

            m_eyeW = desc.Width;
            m_eyeH = desc.Height;
            Game::logMsg("Stereo D3D RTs created %ux%u fmt=%d", m_eyeW, m_eyeH, (int)fmt);
            return D3D_OK;
        }

        D3D9DeviceEx *m_device;
        D3D9DeviceLock m_lock;
        IDirect3DSurface9 *m_left = nullptr;
        IDirect3DSurface9 *m_right = nullptr;
        IDirect3DSurface9 *m_black = nullptr;
        IDirect3DSurface9 *m_overlay = nullptr;
        IDirect3DSurface9 *m_sbs = nullptr;
        IDirect3DStateBlock9 *m_alphaSB = nullptr;
        bool m_alphaSBFailed = false;
        UINT m_eyeW = 0;
        UINT m_eyeH = 0;
        UINT m_overlayW = 0;
        UINT m_overlayH = 0;
        UINT m_sbsW = 0;
        UINT m_sbsH = 0;
    };

}

HRESULT __stdcall Direct3DCreateVRImpl(IDirect3DDevice9 *pDevice, IDirect3DVR9 **pInterface) {
    if (pInterface == nullptr)
        return D3DERR_INVALIDCALL;

    *pInterface = new dxvk::D3D9VR(pDevice);

    return D3D_OK;
}
#include <cstdio>
#include <atomic>
#include <cmath>
#include <algorithm>
#include "../dxvk/dxvk_include.h"

#include "d3d9_vr.h"

#include "d3d9_include.h"
#include "d3d9_surface.h"

#include "d3d9_device.h"

#include "L4D2VR/game.h"
#include "L4D2VR/vr.h"
#include "L4D2VR/vr_guide.h"
#include "L4D2VR/vr_eyediag.h"

#include <vector>
#include <string>
#include <intrin.h>

namespace dxvk {

// ---------------------------------------------------------------------------
// Eye-pass tracing. See vr_eyediag.h for why this exists.
// ---------------------------------------------------------------------------
int  g_GESVR_EyePass = 0;
bool g_GESVR_EyeTrace = false;
bool g_GESVR_DiagForceUpload = false;
bool  g_GESVR_ScopeActive = false;
bool  g_GESVR_ScopeValid[2] = { false, false };
float g_GESVR_ScopeU[2] = { 0.5f, 0.5f };
float g_GESVR_ScopeV[2] = { 0.5f, 0.5f };
float g_GESVR_ScopeR[2] = { 0.0f, 0.0f };
float g_GESVR_ScopeA[2][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
float g_GESVR_ScopeB[2][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
float g_GESVR_ScopeCross[4] = { 0.0f, -1.0f, 1.0f, 0.0f };

namespace {
    unsigned s_traceEyeW = 0, s_traceEyeH = 0;
    int s_traceEvents = 0;

    // Every distinct state a draw ran under in this pass, with a count. There
    // are only ever a handful per pass, so a linear search is fine.
    struct DrawState
    {
        uint32_t rtW, rtH;
        DWORD vx, vy, vw, vh;
        bool scOn;
        LONG sl, st, sr, sb;
        int n;
    };
    std::vector<DrawState> s_traceDraws;

    // H = the 2D HUD pass, M = one whole main-menu frame (EyeDiagMenuSec / EyeDiagDisconnect)
    char EyeChar(int e) { return e == 1 ? 'L' : e == 2 ? 'R' : e == 3 ? 'H' : e == 4 ? 'M' : '-'; }

    // module+offset for a code address, or false if it is not in a module.
    bool GESVR_Where(const void *p, char *out, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !mbi.AllocationBase)
            return false;
        const DWORD prot = mbi.Protect & 0xFF;
        if (!(prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
              prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY))
            return false;
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH))
            return false;
        const char *base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        _snprintf_s(out, n, _TRUNCATE, "%s+0x%X", base,
                    (unsigned)((const char *)p - (const char *)mbi.AllocationBase));
        return true;
    }

    // Who, in the ENGINE, caused this. The binaries omit frame pointers, so this
    // scans the stack for anything that is a code address in a Source module --
    // the same poor-man's unwind the watchdog uses. Our own frames and the OS
    // are skipped; what is left is shaderapidx9 / materialsystem / engine /
    // client, which is the part worth disassembling.
    // Source's own modules only. The first scan kept everything that looked
    // like code and was mostly stale words pointing into the NVIDIA driver,
    // the IME and gameui -- never the engine frames that were wanted.
    bool GESVR_IsSourceModule(const char *desc)
    {
        static const char *kKeep[] = { "engine.dll", "client.dll", "materialsystem.dll",
                                       "shaderapidx9.dll", "stdshader_dx9.dll", "shadereditor.dll",
                                       "game_shader_generic", "studiorender.dll" };
        for (const char *k : kKeep)
            if (!_strnicmp(desc, k, strlen(k)))
                return true;
        // A menu frame (pass M) is drawn by VGUI, so there its modules count.
        static const char *kMenu[] = { "GameUI.dll", "vgui2.dll", "vguimatsurface.dll" };
        if (g_GESVR_EyePass == 4)
            for (const char *k : kMenu)
                if (!_strnicmp(desc, k, strlen(k)))
                    return true;
        return false;
    }

    void GESVR_ScanStack()
    {
        NT_TIB *tib = reinterpret_cast<NT_TIB *>(NtCurrentTeb());
        DWORD_PTR marker = 0;
        DWORD_PTR *sp = &marker;
        DWORD_PTR *top = reinterpret_cast<DWORD_PTR *>(tib->StackBase);
        int printed = 0;
        char desc[200], last[200] = "";
        for (int i = 0; i < 4096 && sp + i < top && printed < 24; ++i)
        {
            DWORD_PTR v = 0;
            __try { v = sp[i]; }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            if (!GESVR_Where(reinterpret_cast<const void *>(v), desc, sizeof(desc)))
                continue;
            if (!GESVR_IsSourceModule(desc) || !strcmp(desc, last))
                continue;
            strcpy_s(last, desc);
            Game::logMsg("EYETRACE         stack [%02d] %s", printed++, desc);
        }
    }
}

void GESVR_EyeTraceNote(const char *text, const void *caller, bool withStack)
{
    if (!g_GESVR_EyeTrace || !g_GESVR_EyePass || s_traceEvents >= 160)
        return;
    ++s_traceEvents;
    char where[200] = "?";
    GESVR_Where(caller, where, sizeof(where));
    Game::logMsg("EYETRACE %c %s  (called from %s)", EyeChar(g_GESVR_EyePass), text, where);
    if (withStack)
        GESVR_ScanStack();
}

// A line in vrmod_log with the caller and a filtered stack, trace or not.
void GESVR_LogWithStack(const char *text, const void *caller)
{
    char where[200] = "?";
    GESVR_Where(caller, where, sizeof(where));
    Game::logMsg("%s  (called from %s)", text, where);
    GESVR_ScanStack();
}

void GESVR_EyeTraceBeginPass(int eye, unsigned eyeW, unsigned eyeH)
{
    g_GESVR_EyePass = eye;
    s_traceEyeW = eyeW;
    s_traceEyeH = eyeH;
    s_traceEvents = 0;
    s_traceDraws.clear();
    if (g_GESVR_EyeTrace)
        Game::logMsg("EYETRACE %c ===== BEGIN pass, target %ux%u =====", EyeChar(eye), eyeW, eyeH);
}

void GESVR_EyeTraceEndPass(int eye)
{
    if (g_GESVR_EyeTrace)
    {
        int total = 0, bad = 0;
        for (const DrawState &d : s_traceDraws)
        {
            total += d.n;
            const bool eyeRT = s_traceEyeW && d.rtW == s_traceEyeW && d.rtH == s_traceEyeH;
            const bool vpShort = d.vx || d.vy || d.vw < d.rtW || d.vh < d.rtH;
            const bool scShort = d.scOn && (d.sl > 0 || d.st > 0 || (uint32_t)d.sr < d.rtW || (uint32_t)d.sb < d.rtH);
            if (eyeRT && (vpShort || scShort))
                bad += d.n;
            Game::logMsg("EYETRACE %c draws x%-5d rt=%ux%u%s vp=(%lu,%lu %lux%lu) scissor %s(%ld,%ld,%ld,%ld)%s",
                         EyeChar(eye), d.n, d.rtW, d.rtH, eyeRT ? "[EYE]" : "",
                         d.vx, d.vy, d.vw, d.vh, d.scOn ? "ON " : "off", d.sl, d.st, d.sr, d.sb,
                         (eyeRT && (vpShort || scShort)) ? "  <-- does not cover the eye target" : "");
        }
        Game::logMsg("EYETRACE %c ===== END pass: %d draws, %d of them clipped short of the eye target =====",
                     EyeChar(eye), total, bad);
    }
    g_GESVR_EyePass = 0;
}

// Called by D3D9DeviceEx::GESVR_TraceState with the device's live state.
void GESVR_TraceRecord(const char *what, const void *caller, bool isDraw,
                       uint32_t rtW, uint32_t rtH, const D3DVIEWPORT9 &vp, const RECT &sc, bool scOn)
{
    if (isDraw)
    {
        for (DrawState &d : s_traceDraws)
        {
            if (d.rtW == rtW && d.rtH == rtH && d.vx == vp.X && d.vy == vp.Y && d.vw == vp.Width &&
                d.vh == vp.Height && d.scOn == scOn && d.sl == sc.left && d.st == sc.top &&
                d.sr == sc.right && d.sb == sc.bottom)
            {
                ++d.n;
                return;
            }
        }
        if (s_traceDraws.size() < 64)
            s_traceDraws.push_back({ rtW, rtH, vp.X, vp.Y, vp.Width, vp.Height, scOn,
                                     sc.left, sc.top, sc.right, sc.bottom, 1 });
        return;
    }

    if (s_traceEvents >= 160)
        return;
    ++s_traceEvents;
    const bool eyeRT = s_traceEyeW && rtW == s_traceEyeW && rtH == s_traceEyeH;
    const bool vpShort = vp.X || vp.Y || vp.Width < rtW || vp.Height < rtH;
    const bool scShort = scOn && (sc.left > 0 || sc.top > 0 || (uint32_t)sc.right < rtW || (uint32_t)sc.bottom < rtH);
    const bool suspect = eyeRT && (vpShort || scShort);
    char where[200] = "?";
    GESVR_Where(caller, where, sizeof(where));
    Game::logMsg("EYETRACE %c %-22s rt=%ux%u%s vp=(%lu,%lu %lux%lu) scissor %s(%ld,%ld,%ld,%ld) by %s%s",
                 EyeChar(g_GESVR_EyePass), what, rtW, rtH, eyeRT ? "[EYE]" : "",
                 vp.X, vp.Y, vp.Width, vp.Height, scOn ? "ON " : "off",
                 sc.left, sc.top, sc.right, sc.bottom, where,
                 suspect ? "  <-- does not cover the eye target" : "");
    if (suspect)
        GESVR_ScanStack();
}

// 24-bit bottom-up BMP from a locked A8R8G8B8 / X8R8G8B8 surface.
static bool GESVR_WriteBmp(const char *path, const uint8_t *bits, int pitch, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    const int row = (w * 3 + 3) & ~3;
    const uint32_t imageSize = (uint32_t)row * (uint32_t)h;
    uint8_t hdr[54] = { 'B', 'M' };
    auto put32 = [&](int off, uint32_t v) { memcpy(hdr + off, &v, 4); };
    auto put16 = [&](int off, uint16_t v) { memcpy(hdr + off, &v, 2); };
    put32(2, 54 + imageSize); put32(10, 54); put32(14, 40);
    put32(18, (uint32_t)w); put32(22, (uint32_t)h); put16(26, 1); put16(28, 24);
    put32(34, imageSize);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (int y = h - 1; y >= 0; --y)
    {
        const uint8_t *src = bits + (size_t)y * pitch;
        for (int x = 0; x < w; ++x)
        {
            line[x * 3 + 0] = src[x * 4 + 0];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    return true;
}

// Set from config: draw a VR aiming reticle into each eye.
bool  g_GESVR_DrawReticle = true;
// Force the menu overlay's alpha opaque. See ForceOpaqueAlpha.
bool  g_GESVR_ForceMenuOpaque = true;
// Swap which surface each eye captures into. A test, not a feature: if the
// black eye follows the SURFACE it is m_right that is bad; if it stays on the
// second eye rendered, the fault is in the second capture's timing instead.
bool  g_GESVR_SwapEyeSurfaces = false;
// Client size of the game window, published by the thread that owns USER32.
// Present must never call USER32 itself: entering it lets the kernel deliver a
// queued window callback on the same thread, which drives a nested present that
// waits on a submission the outer present has not finished issuing.
std::atomic<uint32_t> g_GESVR_ClientW{ 0 };
std::atomic<uint32_t> g_GESVR_ClientH{ 0 };
// Reticle arm length as a fraction of eye-image height. The original was
// height/90 + 4 (~16px arm at 1080), which reads as very large once the eye
// image is magnified across the headset panel. Tunable via VRReticleSize.
float g_GESVR_ReticleScale = 0.0007f;
// Horizontal correction for the reticle. The eye surface is the full 16:9
// backbuffer, and the whole of it is mapped onto a roughly square eye
// viewport, which squashes X relative to Y -- so a circle drawn in pixels
// comes out as a vertically stretched oval. Widening X by the surface aspect
// cancels that. 0 = derive it from the surface; set a number to override.
float g_GESVR_ReticleAspect = 0.0f;
// The eye frustum's aspect (tan-width / tan-height), published by the mod. The
// eye image of W x H pixels is shown across a frustum of this aspect, so a
// pixel is (EyeAspect * H / W) as wide as it is tall and a circle needs its
// horizontal radius multiplied by (W / H) / EyeAspect. The old factor was plain
// W / H, which ignored the frustum: ~4% narrow on the window path (1.78 vs
// 1.85) and on the eye-target path (0.96 vs 1.0). 0 = unknown, use W / H.
float g_GESVR_EyeAspect = 0.0f;

// Throw-guide dot radii are clamped in pixels, and those clamps were tuned on
// 1440-row eye images. Scale them with the image so the guide looks the same
// at any eye resolution -- per-eye targets roughly double the rows.
static const float kGuideTunedRows = 1440.0f;

} // namespace dxvk

// Called from VR::Init once the HMD's projection is known.
float GESVR_PublishEyeAspect(float a)
{
    dxvk::g_GESVR_EyeAspect = a;
    return a;
}

// FakeSubmitOOM test hook (vr_eyediag.h): submits still to fail on purpose.
std::atomic<int> g_GESVR_FakeSubmitOOM{ 0 };

void GESVR_FakeSubmitOOM(int n)
{
    g_GESVR_FakeSubmitOOM.store(n);
}

// DXVK's warnings and errors, copied into vrmod_log.txt (util/log/log.cpp).
// Capped: this DXVK warns a handful of times per launch, so hitting the cap
// means something is repeating and the first lines are the ones that matter.
void GESVR_DxvkLogLine(const char *prefix, const char *line)
{
    static std::atomic<int> s_lines{ 0 };
    const int n = s_lines.fetch_add(1);
    if (n < 300)
        Game::logMsg("DXVK %s%s", prefix, line);
    else if (n == 300)
        Game::logMsg("DXVK: 300 warnings/errors logged; the rest are in hl2_d3d9.log");
}

// A queue thread hit a GPU error it cannot come back from (dxvk_queue.cpp).
// Before this, DXVK froze in its own error handling and the game froze with it,
// which in the headset is a hung image until the player finds Task Manager.
// Ending the process says why and lets them relaunch straight away.
void GESVR_DxvkFatal(const char *what, int vkResult)
{
    const char *name = vkResult == -1 ? "out of host memory (in this 32-bit process, usually address space)"
                     : vkResult == -2 ? "out of device memory"
                     : vkResult == -4 ? "device lost"
                     : "unexpected error";
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    const unsigned usedMB = (unsigned)((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20);
    // The whole-address-space walk is too slow for every frame but fine here.
    SIZE_T hole = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t p = 0x10000; p < 0x7FFF0000u && VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == sizeof(mbi);
         p = (uintptr_t)mbi.BaseAddress + mbi.RegionSize)
    {
        if (mbi.State == MEM_FREE && mbi.RegionSize > hole)
            hole = mbi.RegionSize;
    }
    Game::logMsg("DXVK FATAL: %s returned VkResult %d, %s. Address space %u of %u MB in use, "
                 "largest free block %u MB, in map %d. The GPU queue cannot recover from this, "
                 "so the game is being closed instead of left frozen.",
                 what, vkResult, name, usedMB, (unsigned)(ms.ullTotalVirtual >> 20),
                 (unsigned)(hole >> 20), (int)GESVRMem::g_inMap.load());
    TerminateProcess(GetCurrentProcess(), 0xE0DE0000u | (unsigned)(-vkResult & 0xFF));
}

namespace dxvk {

static float GESVR_RoundFactor(UINT w, UINT h)
{
    if (g_GESVR_ReticleAspect > 0.0f)
        return g_GESVR_ReticleAspect;
    float ar = (h > 0) ? (float)w / (float)h : 1.0f;
    if (g_GESVR_EyeAspect > 0.1f && g_GESVR_EyeAspect < 10.0f)
        ar /= g_GESVR_EyeAspect;
    if (ar < 0.25f) ar = 0.25f;
    if (ar > 4.0f)  ar = 4.0f;
    return ar;
}
// 0 = cross, 1 = dot, 2 = ring, 3 = ring + dot. A dot is far less intrusive at
// the centre of view.
int   g_GESVR_ReticleStyle = 1;
// Index into kReticleColors: yellow, white, green, red, cyan.
int   g_GESVR_ReticleColor = 0;
// Tracked gun: draw the reticle where the barrel's aim point lands in each
// eye (normalised image coordinates), not at the centre. Hidden for an eye
// whose point is not valid (behind it, or no gun held).
// Set while zoomed in: the reticle shows even with VRReticle off.
bool  g_GESVR_ReticleForce = false;
bool  g_GESVR_ReticleUseAim = false;
bool  g_GESVR_ReticleAimValid[2] = { false, false };
float g_GESVR_ReticleAimU[2] = { 0.5f, 0.5f };
float g_GESVR_ReticleAimV[2] = { 0.5f, 0.5f };
static const D3DCOLOR kReticleColors[] = {
    D3DCOLOR_ARGB(255, 255, 245, 120),
    D3DCOLOR_ARGB(255, 255, 255, 255),
    D3DCOLOR_ARGB(255, 90, 255, 110),
    D3DCOLOR_ARGB(255, 255, 70, 60),
    D3DCOLOR_ARGB(255, 80, 230, 255),
};
// The throw guide (see L4D2VR/vr_guide.h and VR::UpdateThrowGuide).
int g_GESVR_GuideCount[2] = { 0, 0 };
GESVR_GuideDot g_GESVR_Guide[2][kGESVRGuideMax];

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

        // Out of a map these are dead weight: two window-sized render targets
        // (2560x1440 A8R8G8B8 is 14.7 MB each) plus the side-by-side buffer at
        // double width, all held from the first map until the process exits.
        // A map load is exactly when that space is worth most -- loads have
        // died here with 75 MB free and a largest hole of 32 MB.
        //
        // No GPU wait: DXVK defers the real destruction until the images are
        // idle, and waiting for the GPU from inside PresentEx is the shape of
        // every deadlock this mod has had. The caller stops submitting these
        // several frames before calling, which covers the compositor.
        // The sniper scope's view: copy the current render target (the scope
        // pass just drew it) and cross it with a thin black crosshair. The eye
        // captures then paint it into the lens.
        HRESULT STDMETHODCALLTYPE CaptureScopeRT()
        {
            IDirect3DSurface9 *src = nullptr;
            HRESULT hr = m_device->GetRenderTarget(0, &src);
            if (FAILED(hr) || !src)
                return FAILED(hr) ? hr : E_FAIL;
            D3DSURFACE_DESC sd{};
            src->GetDesc(&sd);
            if (!m_scope)
                hr = m_device->CreateRenderTarget(sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &m_scope, nullptr);
            if (SUCCEEDED(hr))
                hr = m_device->StretchRect(src, nullptr, m_scope, nullptr, D3DTEXF_NONE);
            src->Release();
            if (FAILED(hr))
                return hr;
            const LONG w = (LONG)sd.Width, h = (LONG)sd.Height;
            const LONG t = (h / 400 > 1) ? h / 400 : 1;   // ~2-3 px at 1024
            const LONG gap = h / 40;                      // clear centre
            const D3DCOLOR black = D3DCOLOR_ARGB(255, 0, 0, 0);
            // The picture is level with the head (VR::UpdateGunAim); the cross
            // follows the gun, so it is drawn along the gun's up and right as
            // seen in this picture. Built from t x t fills, one per t pixels
            // along each arm -- about 1000 small fills a frame, scoped only.
            const float cxf = 0.5f * (float)w, cyf = 0.5f * (float)h;
            const float dirs[2][2] = { { g_GESVR_ScopeCross[0], g_GESVR_ScopeCross[1] },
                                       { g_GESVR_ScopeCross[2], g_GESVR_ScopeCross[3] } };
            for (const auto &d : dirs)
            {
                const float len = sqrtf(d[0] * d[0] + d[1] * d[1]);
                if (len < 0.01f)
                    continue;
                const float ux = d[0] / len, uy = d[1] / len;
                for (LONG k = gap; k < w / 2; k += t)
                {
                    for (int side = -1; side <= 1; side += 2)
                    {
                        const LONG px = (LONG)(cxf + side * ux * (float)k), py = (LONG)(cyf + side * uy * (float)k);
                        RECT r = { px - t / 2, py - t / 2, px - t / 2 + t, py - t / 2 + t };
                        if (r.left < 0 || r.top < 0 || r.right > w || r.bottom > h)
                            continue;
                        m_device->ColorFill(m_scope, &r, black);
                    }
                }
            }
            const RECT dot = { w / 2 - t, h / 2 - t, w / 2 + t, h / 2 + t };
            m_device->ColorFill(m_scope, &dot, D3DCOLOR_ARGB(255, 255, 40, 40));
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE ReleaseEyeSurfaces()
        {
            if (!m_left && !m_right && !m_sbs)
                return S_FALSE;

            const UINT w = m_eyeW, h = m_eyeH;
            if (m_left)  { m_left->Release();  m_left = nullptr; }
            if (m_right) { m_right->Release(); m_right = nullptr; }
            if (m_sbs)   { m_sbs->Release();   m_sbs = nullptr; }
            if (m_scope) { m_scope->Release(); m_scope = nullptr; }
            m_eyeW = m_eyeH = 0;
            m_sbsW = m_sbsH = 0;      // so the size check rebuilds them
            Game::logMsg("Stereo D3D RTs released (were %ux%u)", w, h);
            return D3D_OK;
        }

        // Two-phase eye snapshot; see the interface. Each eye is first scaled
        // into a small render target so the system-memory copies stay a few MB
        // -- full-size copies of both eyes would be ~60 MB in a process that is
        // already short of address space.
        HRESULT STDMETHODCALLTYPE DiagEyeDumpIssue(UINT maxWidth)
        {
            // [2] is the menu overlay surface -- what the floating menu panel
            // shows -- so a broken menu can be looked at, not just the eyes.
            // [3] is the backbuffer as it is at the moment of the call -- right
            // after the HUD pass when called from the end of dRenderView.
            IDirect3DSurface9 *bbNow = nullptr;
            m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bbNow);
            IDirect3DSurface9 *src[5] = { m_left, m_right, m_overlay, bbNow, m_scope };
            HRESULT res[5] = { E_FAIL, E_FAIL, E_FAIL, E_FAIL, E_FAIL };
            for (int e = 0; e < 5; ++e)
            {
                if (m_diagRT[e])  { m_diagRT[e]->Release();  m_diagRT[e] = nullptr; }
                if (m_diagMem[e]) { m_diagMem[e]->Release(); m_diagMem[e] = nullptr; }
                if (!src[e])
                    continue;
                D3DSURFACE_DESC d{};
                if (FAILED(src[e]->GetDesc(&d)) || d.Width == 0 || d.Height == 0)
                    continue;
                UINT w = d.Width, h = d.Height;
                if (maxWidth && w > maxWidth) { h = (UINT)((unsigned long long)h * maxWidth / w); w = maxWidth; }
                if (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8)
                {
                    Game::logMsg("EYEDUMP eye %d format %d is not 8888; skipped", e, (int)d.Format);
                    continue;
                }
                HRESULT hr = m_device->CreateRenderTarget(w, h, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &m_diagRT[e], nullptr);
                if (SUCCEEDED(hr))
                    hr = m_device->StretchRect(src[e], nullptr, m_diagRT[e], nullptr, D3DTEXF_LINEAR);
                if (SUCCEEDED(hr))
                    hr = m_device->CreateOffscreenPlainSurface(w, h, d.Format, D3DPOOL_SYSTEMMEM, &m_diagMem[e], nullptr);
                if (SUCCEEDED(hr))
                    hr = m_device->GetRenderTargetData(m_diagRT[e], m_diagMem[e]);
                res[e] = hr;
                Game::logMsg("EYEDUMP eye %d source %ux%u -> %ux%u queued hr=0x%08X",
                             e, d.Width, d.Height, w, h, (unsigned)hr);
            }
            if (bbNow)
                bbNow->Release();
            return SUCCEEDED(res[0]) && SUCCEEDED(res[1]) ? D3D_OK : E_FAIL;
        }

        HRESULT STDMETHODCALLTYPE DiagEyeDumpWrite(const char *pathPrefix)
        {
            static const char *kSide[5] = { "L", "R", "O", "B", "S" };
            for (int e = 0; e < 5; ++e)
            {
                if (!m_diagMem[e])
                    continue;
                D3DSURFACE_DESC d{};
                m_diagMem[e]->GetDesc(&d);
                D3DLOCKED_RECT lr{};
                HRESULT hr = m_diagMem[e]->LockRect(&lr, nullptr, D3DLOCK_READONLY);
                if (SUCCEEDED(hr))
                {
                    std::string path = std::string(pathPrefix) + "_" + kSide[e] + ".bmp";
                    const bool ok = GESVR_WriteBmp(path.c_str(), (const uint8_t *)lr.pBits, lr.Pitch,
                                                   (int)d.Width, (int)d.Height);
                    m_diagMem[e]->UnlockRect();
                    Game::logMsg("EYEDUMP wrote %s (%ux%u) %s", path.c_str(), d.Width, d.Height, ok ? "ok" : "FAILED");
                }
                else
                    Game::logMsg("EYEDUMP eye %d LockRect failed 0x%08X", e, (unsigned)hr);
                m_diagMem[e]->Release(); m_diagMem[e] = nullptr;
                if (m_diagRT[e]) { m_diagRT[e]->Release(); m_diagRT[e] = nullptr; }
            }
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

            // Size the eye surfaces to the SOURCE, so a full-surface capture keeps
            // its resolution instead of being squeezed into the window size.
            UINT wantW = 0, wantH = 0;
            {
                D3DSURFACE_DESC sd{};
                if (SUCCEEDED(src->GetDesc(&sd)))
                {
                    if (srcRect)
                    {
                        const LONG rw = srcRect->right - srcRect->left;
                        const LONG rh = srcRect->bottom - srcRect->top;
                        if (rw > 0 && rh > 0) { wantW = (UINT)rw; wantH = (UINT)rh; }
                    }
                    else
                    {
                        wantW = sd.Width; wantH = sd.Height;
                    }
                }
            }
            HRESULT hr = EnsureStereoSurfaces(wantW, wantH);
            if (FAILED(hr))
                return hr;

            const bool useLeftSurface = g_GESVR_SwapEyeSurfaces ? (eye != 0) : (eye == 0);
            IDirect3DSurface9 *dest = useLeftSurface ? m_left : m_right;
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
            const int aimEye = (eye == 0) ? 0 : 1;
            const bool aimHidden = g_GESVR_ReticleUseAim && !g_GESVR_ReticleAimValid[aimEye];
            if ((g_GESVR_DrawReticle || g_GESVR_ReticleForce) && !aimHidden)
            {
                D3DSURFACE_DESC dd{};
                if (SUCCEEDED(dest->GetDesc(&dd)) && dd.Width > 32 && dd.Height > 32)
                {
                    LONG cx = (LONG)(dd.Width / 2);
                    LONG cy = (LONG)(dd.Height / 2);
                    if (g_GESVR_ReticleUseAim)
                    {
                        cx = (LONG)(g_GESVR_ReticleAimU[aimEye] * (float)dd.Width);
                        cy = (LONG)(g_GESVR_ReticleAimV[aimEye] * (float)dd.Height);
                    }
                    float sc = g_GESVR_ReticleScale;
                    if (sc < 0.001f) sc = 0.001f;
                    if (sc > 0.200f) sc = 0.200f;
                    // Keep this sub-pixel: at the old 3px floor the reticle was
                    // pinned to the floor, so shrinking VRReticleSize did nothing.
                    float armF = (float)dd.Height * sc;
                    if (armF < 0.6f) armF = 0.6f;
                    LONG arm = (LONG)(armF + 0.5f);
                    if (arm < 1) arm = 1;
                    LONG th = arm / 5;
                    if (th < 1) th = 1;
                    const int ci = g_GESVR_ReticleColor;
                    const D3DCOLOR col = kReticleColors[(ci >= 0 && ci < 5) ? ci : 0];
                    const float ar = GESVR_RoundFactor(dd.Width, dd.Height);

                    // ColorFill only draws rectangles, so round shapes are built
                    // from one fill per scanline. Horizontal radii are widened by
                    // the aspect so they land on screen as circles, not ovals.
                    auto span = [&](LONG x0, LONG x1, LONG y) {
                        RECT row = { x0, y, x1, y + 1 };
                        if (row.left < 0) row.left = 0;
                        if (row.top  < 0) row.top  = 0;
                        if (row.right  > (LONG)dd.Width)  row.right  = (LONG)dd.Width;
                        if (row.bottom > (LONG)dd.Height) row.bottom = (LONG)dd.Height;
                        if (row.right > row.left && row.bottom > row.top)
                            m_device->ColorFill(dest, &row, col);
                    };
                    // Half-width of a circle of radius r at row offset dy, or -1 outside it.
                    auto halfWidth = [&](float r, LONG dy) -> LONG {
                        const float ny = (float)dy / (r + 0.5f);
                        const float inside = 1.0f - ny * ny;
                        if (inside <= 0.0f)
                            return -1;
                        return (LONG)(r * ar * sqrtf(inside) + 0.5f);
                    };
                    auto disc = [&](float r) {
                        const LONG ir = (LONG)(r + 0.5f);
                        for (LONG dy = -ir; dy <= ir; ++dy)
                        {
                            const LONG hw = halfWidth(r, dy);
                            if (hw >= 1)
                                span(cx - hw, cx + hw, cy + dy);
                        }
                    };
                    auto ring = [&](float outer, float thick) {
                        const float inner = outer - thick;
                        const LONG io = (LONG)(outer + 0.5f);
                        for (LONG dy = -io; dy <= io; ++dy)
                        {
                            const LONG ho = halfWidth(outer, dy);
                            if (ho < 1)
                                continue;
                            const LONG hi = (inner > 0.5f) ? halfWidth(inner, dy) : -1;
                            if (hi < 1)
                                span(cx - ho, cx + ho, cy + dy);
                            else
                            {
                                span(cx - ho, cx - hi, cy + dy);
                                span(cx + hi, cx + ho, cy + dy);
                            }
                        }
                    };

                    const float thick = (armF * 0.7f < 1.0f) ? 1.0f : armF * 0.7f;
                    // Filled box, clipped to the surface.
                    auto box = [&](LONG x0, LONG y0, LONG x1, LONG y1) {
                        RECT r = { x0, y0, x1, y1 };
                        if (r.left < 0) r.left = 0;
                        if (r.top  < 0) r.top  = 0;
                        if (r.right  > (LONG)dd.Width)  r.right  = (LONG)dd.Width;
                        if (r.bottom > (LONG)dd.Height) r.bottom = (LONG)dd.Height;
                        if (r.right > r.left && r.bottom > r.top)
                            m_device->ColorFill(dest, &r, col);
                    };
                    switch (g_GESVR_ReticleStyle)
                    {
                    case 4:
                    {
                        // Classic: GoldenEye's crosshair (GE:S sprites/crosshair)
                        // -- a ring with four spikes tapering in towards a clear
                        // centre, the spikes poking just past the ring. Much
                        // bigger than the dot for the same size setting so the
                        // spikes survive at the smallest size.
                        float R = armF * 6.0f;
                        if (R < 6.0f) R = 6.0f;
                        if (R > (float)dd.Height * 0.25f) R = (float)dd.Height * 0.25f;
                        const float t = (R * 0.12f < 1.0f) ? 1.0f : R * 0.12f;
                        ring(R, t);
                        const float tip = R * 0.28f, inner = R - t, nub = R * 1.10f;
                        const float w0 = (R * 0.09f < 1.0f) ? 1.0f : R * 0.09f;
                        // Half-thickness of a spike at distance d from the centre.
                        auto spike = [&](float d) -> float {
                            if (d > inner)
                                return w0 * 0.7f;
                            const float k = (d - tip) / (inner - tip);
                            return (k < 0.0f ? 0.0f : k) * w0;
                        };
                        // Top and bottom: one row at a time.
                        for (LONG d = (LONG)tip; d <= (LONG)nub; ++d)
                        {
                            const LONG hw = (LONG)(spike((float)d) * ar + 0.5f);
                            box(cx - hw, cy - d, cx + hw + 1, cy - d + 1);
                            box(cx - hw, cy + d, cx + hw + 1, cy + d + 1);
                        }
                        // Left and right: one column at a time, widened by the
                        // aspect like the ring so the shape stays round.
                        for (LONG px = (LONG)(tip * ar); px <= (LONG)(nub * ar); ++px)
                        {
                            const LONG hh = (LONG)(spike((float)px / ar) + 0.5f);
                            box(cx - px, cy - hh, cx - px + 1, cy + hh + 1);
                            box(cx + px, cy - hh, cx + px + 1, cy + hh + 1);
                        }
                        break;
                    }
                    case 1:
                        disc(armF);
                        break;
                    case 2:
                        ring(armF * 3.0f, thick);
                        break;
                    case 3:
                        ring(armF * 3.0f, thick);
                        disc(armF * 0.8f);
                        break;
                    default:
                    {
                        const LONG armX = (LONG)((float)arm * ar);
                        const LONG thX  = (LONG)((float)th * ar) > 0 ? (LONG)((float)th * ar) : 1;
                        RECT hr2 = { cx - armX, cy - th, cx + armX, cy + th };
                        RECT vr2 = { cx - thX, cy - arm, cx + thX, cy + arm };
                        m_device->ColorFill(dest, &hr2, col);
                        m_device->ColorFill(dest, &vr2, col);
                        break;
                    }
                    }
                }
            }

            // The sniper scope lens: a disc across the end of the scope, square
            // to the barrel, so it projects to an ellipse (centre + s*A + t*B,
            // s^2 + t^2 <= 1; see VR::UpdateGunAim). Filled one screen row at a
            // time, each row a StretchRect from the matching strip of the scope
            // view, so only the glass is drawn and the gun and world around it
            // stay. Drawn after the reticle so the dot never shows on the glass.
            {
                const int si = (eye == 0) ? 0 : 1;
                D3DSURFACE_DESC sd{}, td{};
                if (g_GESVR_ScopeActive && g_GESVR_ScopeValid[si] && m_scope && SUCCEEDED(dest->GetDesc(&sd))
                    && SUCCEEDED(m_scope->GetDesc(&td)))
                {
                    const float W = (float)sd.Width, H = (float)sd.Height;
                    const float cx = g_GESVR_ScopeU[si] * W, cy = g_GESVR_ScopeV[si] * H;
                    const float ax = g_GESVR_ScopeA[si][0] * W, ay = g_GESVR_ScopeA[si][1] * H;
                    const float bx = g_GESVR_ScopeB[si][0] * W, by = g_GESVR_ScopeB[si][1] * H;
                    const float det = ax * by - bx * ay;
                    const float halfH = sqrtf(ay * ay + by * by);
                    if (fabsf(det) > 4.0f && halfH >= 2.0f && halfH < H)
                    {
                        // s and t along a row: s = s1*X + s0, t = t1*X + t0, X = x - cx.
                        const float s1 = by / det, t1 = -ay / det;
                        const float TW = (float)td.Width, TH = (float)td.Height;
                        const LONG y0 = std::max<LONG>(0, (LONG)floorf(cy - halfH));
                        const LONG y1 = std::min<LONG>((LONG)sd.Height, (LONG)ceilf(cy + halfH));
                        for (LONG y = y0; y < y1; ++y)
                        {
                            const float dy = (float)y + 0.5f - cy;
                            const float s0 = -bx * dy / det, t0 = ax * dy / det;
                            const float qa = s1 * s1 + t1 * t1;
                            const float qb = 2.0f * (s1 * s0 + t1 * t0);
                            const float qc = s0 * s0 + t0 * t0 - 1.0f;
                            const float disc = qb * qb - 4.0f * qa * qc;
                            if (qa <= 0.0f || disc <= 0.0f)
                                continue;
                            const float root = sqrtf(disc);
                            float X0 = (-qb - root) / (2.0f * qa), X1 = (-qb + root) / (2.0f * qa);
                            LONG x0 = (LONG)ceilf(cx + X0), x1 = (LONG)floorf(cx + X1);
                            x0 = std::max<LONG>(x0, 0);
                            x1 = std::min<LONG>(x1, (LONG)sd.Width);
                            if (x1 - x0 < 1)
                                continue;
                            // Texture at the two ends of the row's span.
                            const float Xa = (float)x0 + 0.5f - cx, Xb = (float)x1 - 0.5f - cx;
                            const float ua = 0.5f + 0.5f * (s1 * Xa + s0), ub = 0.5f + 0.5f * (s1 * Xb + s0);
                            const float va = 0.5f - 0.5f * (t1 * Xa + t0), vb = 0.5f - 0.5f * (t1 * Xb + t0);
                            if (ub <= ua)
                                continue;                     // seen from the front of the lens
                            RECT src = { (LONG)(std::clamp(ua, 0.0f, 1.0f) * TW), 0,
                                         (LONG)(std::clamp(ub, 0.0f, 1.0f) * TW), 0 };
                            src.top = std::min<LONG>((LONG)(std::clamp(0.5f * (va + vb), 0.0f, 1.0f) * TH), (LONG)td.Height - 1);
                            src.bottom = src.top + 1;
                            if (src.right <= src.left)
                                src.right = src.left + 1;
                            RECT dst = { x0, y, x1, y + 1 };
                            m_device->StretchRect(m_scope, &src, dest, &dst, D3DTEXF_LINEAR);
                            // A thin dark edge where the glass meets the tube.
                            const D3DCOLOR edge = D3DCOLOR_ARGB(255, 8, 8, 8);
                            RECT l = { x0, y, std::min(x0 + 2, x1), y + 1 };
                            RECT r = { std::max(x1 - 2, x0), y, x1, y + 1 };
                            m_device->ColorFill(dest, &l, edge);
                            m_device->ColorFill(dest, &r, edge);
                        }
                    }
                }
            }

            // The throw guide: its own dots, drawn whether or not the reticle
            // is on (VR::UpdateThrowGuide leaves the count at 0 when the guide
            // is off). A tiny dot is one fill; bigger ones are built a scanline
            // at a time like the reticle's disc, widened by the same surface
            // aspect so they land in the headset round, not stretched.
            {
                const int gi = (eye == 0) ? 0 : 1;
                const int count = g_GESVR_GuideCount[gi];
                D3DSURFACE_DESC gd{};
                if (count > 0 && SUCCEEDED(dest->GetDesc(&gd)) && gd.Width > 32 && gd.Height > 32)
                {
                    const float gar = GESVR_RoundFactor(gd.Width, gd.Height);
                    const float rowScale = (float)gd.Height / kGuideTunedRows;
                    const int ci = g_GESVR_ReticleColor;
                    const D3DCOLOR cols[3] = { kReticleColors[(ci >= 0 && ci < 5) ? ci : 0],
                                               D3DCOLOR_ARGB(255, 115, 115, 115),
                                               D3DCOLOR_ARGB(255, 255, 70, 60) };
                    const LONG gw = (LONG)gd.Width, gh = (LONG)gd.Height;
                    auto fill = [&](LONG x0, LONG y0, LONG x1, LONG y1, D3DCOLOR col) {
                        RECT rc = { x0 < 0 ? 0 : x0, y0 < 0 ? 0 : y0, x1 > gw ? gw : x1, y1 > gh ? gh : y1 };
                        if (rc.right > rc.left && rc.bottom > rc.top)
                            m_device->ColorFill(dest, &rc, col);
                    };
                    for (int i = 0; i < count && i < kGESVRGuideMax; ++i)
                    {
                        const GESVR_GuideDot &d = g_GESVR_Guide[gi][i];
                        const LONG cx = (LONG)(d.u * (float)gw), cy = (LONG)(d.v * (float)gh);
                        if (cx < -8 || cy < -8 || cx > gw + 8 || cy > gh + 8)
                            continue;
                        float r = d.r * (float)gh;
                        if (r < 0.55f * rowScale) r = 0.55f * rowScale;
                        if (r > 4.5f * rowScale) r = 4.5f * rowScale;
                        const D3DCOLOR col = cols[(d.c >= 0 && d.c < 3) ? d.c : 0];
                        if (r < 1.6f)
                        {
                            const LONG hw = (LONG)(r * gar + 0.5f), hh = (LONG)(r + 0.5f);
                            fill(cx - hw, cy - hh, cx + hw + 1, cy + hh + 1, col);
                            continue;
                        }
                        const LONG ir = (LONG)(r + 0.5f);
                        for (LONG dy = -ir; dy <= ir; ++dy)
                        {
                            const float ny = (float)dy / (r + 0.5f);
                            const float inside = 1.0f - ny * ny;
                            if (inside <= 0.0f)
                                continue;
                            const LONG hw = (LONG)(r * gar * sqrtf(inside) + 0.5f);
                            fill(cx - hw, cy + dy, cx + hw + 1, cy + dy + 1, col);
                        }
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

            // What are we actually capturing? If the right eye reports the
            // backbuffer size rather than the eye texture size, the material
            // system's SetRenderTarget did not take for the second pass and the
            // right eye was never drawn into -- which is what 'mostly black'
            // looks like. Logged for the first few passes only.
            {
                static int s_logged = 0;
                if (s_logged < 8)
                {
                    ++s_logged;
                    D3DSURFACE_DESC sd{};
                    if (SUCCEEDED(src->GetDesc(&sd)))
                        Game::logMsg("CaptureCurrentRT eye=%d source RT is %ux%u",
                                     eye, sd.Width, sd.Height);
                }
            }

            // The pixel-readback probe that lived here is REMOVED. It answered its
            // question -- both eye targets come back fully drawn, so the engine
            // renders both eyes and the right eye's black frame happens after this
            // point, in capture or submit -- but reading a render target back to
            // system memory forces a blocking GPU sync, and doing that from inside
            // the render path froze the game on the stereo pass every time. If that
            // measurement is ever needed again it belongs off the render thread.

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

        HRESULT STDMETHODCALLTYPE FillBackBufferFromEye(float u0, float v0, float u1, float v1)
        {
            if (!m_left)
                return D3DERR_INVALIDCALL;
            IDirect3DSurface9 *bb = nullptr;
            HRESULT hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
            if (FAILED(hr) || !bb)
                return FAILED(hr) ? hr : E_FAIL;
            D3DSURFACE_DESC bd{}, ed{};
            bb->GetDesc(&bd);
            m_left->GetDesc(&ed);
            // The biggest window-aspect rectangle inside the bounds, centred on
            // them. The eye is ~square, so that is a full-width horizontal band.
            const float x0 = u0 * ed.Width, x1 = u1 * ed.Width;
            const float y0 = v0 * ed.Height, y1 = v1 * ed.Height;
            const float want = float(bd.Width) / float(bd.Height);
            float w = x1 - x0, h = y1 - y0;
            if (w <= 1.0f || h <= 1.0f)
            {
                bb->Release();
                return D3DERR_INVALIDCALL;
            }
            if (w / h > want)
                w = h * want;
            else
                h = w / want;
            const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
            RECT src = { LONG(cx - 0.5f * w), LONG(cy - 0.5f * h), LONG(cx + 0.5f * w), LONG(cy + 0.5f * h) };
            src.left = std::max<LONG>(src.left, 0);
            src.top = std::max<LONG>(src.top, 0);
            src.right = std::min<LONG>(src.right, LONG(ed.Width));
            src.bottom = std::min<LONG>(src.bottom, LONG(ed.Height));
            hr = m_device->StretchRect(m_left, &src, bb, nullptr, D3DTEXF_LINEAR);
            bb->Release();
            return hr;
        }

    private:
        // wantW/wantH: the size the eye surfaces should be. Zero means 'match the
        // backbuffer', which is the old behaviour. When rendering into dedicated eye
        // render targets these must match THOSE, or the capture blit quietly
        // downscales a high-resolution eye back to the window size and the whole
        // point of the eye targets is lost.
        HRESULT EnsureStereoSurfaces(UINT wantW = 0, UINT wantH = 0)
        {
            IDirect3DSurface9 *bb = nullptr;
            HRESULT hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
            if (FAILED(hr) || !bb)
                return hr;

            D3DSURFACE_DESC desc{};
            bb->GetDesc(&desc);
            bb->Release();

            if (wantW != 0 && wantH != 0)
            {
                desc.Width  = wantW;
                desc.Height = wantH;
            }

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
                Game::logMsg("CreateRenderTarget left eye failed hr=0x%08X fmt=%d size=%ux%u", (unsigned)hr, (int)fmt, desc.Width, desc.Height);
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
            {
                MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
                GlobalMemoryStatusEx(&ms);
                Game::logMsg("Stereo D3D RTs created %ux%u fmt=%d (address space now %u MB used)", m_eyeW, m_eyeH, (int)fmt,
                             (unsigned)((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20));
            }
            return D3D_OK;
        }

        D3D9DeviceEx *m_device;
        D3D9DeviceLock m_lock;
        IDirect3DSurface9 *m_left = nullptr;
        IDirect3DSurface9 *m_right = nullptr;
        IDirect3DSurface9 *m_black = nullptr;
        IDirect3DSurface9 *m_overlay = nullptr;
        IDirect3DSurface9 *m_sbs = nullptr;
        IDirect3DSurface9 *m_scope = nullptr;     // sniper scope view, see CaptureScopeRT
        IDirect3DSurface9 *m_diagRT[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };    // DiagEyeDump only
        IDirect3DSurface9 *m_diagMem[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
        IDirect3DStateBlock9 *m_alphaSB = nullptr;
        bool m_alphaSBFailed = false;
        UINT m_eyeW = 0;
        UINT m_eyeH = 0;
        UINT m_overlayW = 0;
        UINT m_overlayH = 0;
        UINT m_sbsW = 0;
        UINT m_sbsH = 0;
    };

void GESVR_EyeTraceSnapshot(const char *label)
{
    if (!g_GESVR_EyeTrace || !g_D3DVR9)
        return;
    IDirect3DDevice9 *dev = g_D3DVR9->GetD3DDevice();
    if (dev)
        static_cast<D3D9DeviceEx *>(dev)->GESVR_TraceState(label, _ReturnAddress(), false);
}

}

HRESULT __stdcall Direct3DCreateVRImpl(IDirect3DDevice9 *pDevice, IDirect3DVR9 **pInterface) {
    if (pInterface == nullptr)
        return D3DERR_INVALIDCALL;

    *pInterface = new dxvk::D3D9VR(pDevice);

    return D3D_OK;
}
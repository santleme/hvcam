// mf_capture.cpp — Media Foundation async capture.
//
// Windows-only (MSVC). Not yet exercised on hardware — the API path
// is documented: MFEnumDeviceSources -> ActivateObject ->
// MFCreateSourceReaderFromMediaSource(async) ->
// GetNativeMediaType/SetCurrentMediaType -> ReadSample pump.
//
// NOTE ON HARDWARE VALIDATION: the first hardware run must confirm
// (a) subtype GUIDs mapping, (b) IAMCameraControl/IAMVideoProcAmp QI
// path on the target UVC driver — some drivers expose them, some
// don't; failures degrade to warnings, never to capture loss.
#include "mf_capture.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <strmif.h> // IAMCameraControl / IAMVideoProcAmp
#include <ks.h>
#include <ksproxy.h>
#include <vidcap.h>
#include <combaseapi.h>
#include <initguid.h>
#include <windows.h>

#include <atomic>
#include <climits>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "frame_slots.h"
#include "protocol.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "strmiids.lib")

namespace {

void check_hr(HRESULT hr, const char *what)
{
    if (FAILED(hr)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s (hr=0x%08lx)", what, (unsigned long)hr);
        throw std::runtime_error(msg);
    }
}

uint32_t subtype_to_fmt(const GUID &sub)
{
    if (sub == MFVideoFormat_YUY2) return HVCAM_FMT_YUY2;
    if (sub == MFVideoFormat_NV12) return HVCAM_FMT_NV12;
    if (sub == MFVideoFormat_MJPG) return HVCAM_FMT_MJPEG;
    if (sub == MFVideoFormat_RGB24) return HVCAM_FMT_BGR24;
    if (sub == MFVideoFormat_RGB32) return HVCAM_FMT_BGRX32; // 4B/px declarado
    return 0;
}

// Higher score = better. Exact size match dominates; uncompressed
// formats beat MJPG only when the caller wants ready pixels.
int score_type(const GUID &sub, UINT32 w, UINT32 h, UINT32 fpsNum, UINT32 fpsDen,
               const MfOpenOptions &o)
{
    int score = 0;
    if (o.width && w == (UINT32)o.width) score += 10000;
    if (o.height && h == (UINT32)o.height) score += 10000;
    if (o.width && w != (UINT32)o.width) score -= (int)(w > (UINT32)o.width ? 200 : 50);
    if (o.height && h != (UINT32)o.height) score -= 200;
    if (o.fps && fpsDen && (fpsNum / fpsDen) == (UINT32)o.fps) score += 1000;
    uint32_t f = subtype_to_fmt(sub);
    if (!f) return -1;
    if (o.prefer_format && f == o.prefer_format) score += 500;
    if (f == HVCAM_FMT_YUY2) score += 30;   // ready pixels, cheap USB
    else if (f == HVCAM_FMT_NV12) score += 25;
    else if (f == HVCAM_FMT_MJPEG) score += 20; // smallest payload
    else if (f == HVCAM_FMT_BGR24) score += 10;
    return score;
}

// IMFSourceReaderCallback implementation — plain COM, no ATL.
class ReaderCallback final : public IMFSourceReaderCallback {
public:
    ReaderCallback() : ref_(1) {}

    void attach(IMFSourceReader *reader, FrameSlots *slots,
                int w, int h, uint32_t fmt)
    {
        reader_ = reader;
        slots_ = slots;
        w_ = w;
        h_ = h;
        fmt_ = fmt;
    }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFSourceReaderCallback) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IMFSourceReaderCallback
    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT hrStatus,
        DWORD streamIndex, DWORD streamFlags, LONGLONG timestamp,
        IMFSample *sample) override
    {
        (void)streamIndex;
        if (FAILED(hrStatus)) {
            // Erro de leitura: rearma a bomba — um erro transiente
            // nao pode congelar o stream. Passou de 100 falhas
            // seguidas, marca terminal (a camera provavelmente morreu).
            if (++read_errors_ > 100) {
                if (!ended_.exchange(true))
                    fprintf(stderr, "hvcam: leituras falhando em sequencia; captura encerrada\n");
                return S_OK;
            }
            if (reader_)
                reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                    0, nullptr, nullptr, nullptr, nullptr);
            return S_OK;
        }
        if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            fprintf(stderr, "hvcam: fluxo da camera encerrou\n");
            ended_.store(true);
            return S_OK;
        }
        read_errors_ = 0;
        if (sample) {
            IMFMediaBuffer *buf = nullptr;
            HRESULT hr = sample->ConvertToContiguousBuffer(&buf);
            if (SUCCEEDED(hr) && buf) {
                BYTE *data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&data, nullptr, &len)) && data && len) {
                    HvSlot *slot = slots_->acquire_write();
                    if (slot) {
                        slot->bytes.assign(data, data + len);
                        slot->w = (uint16_t)w_;
                        slot->h = (uint16_t)h_;
                        slot->format = fmt_;
                        slot->seq = seq_++;
                        // Relogio unico: QPC na chegada, sempre.
                        // Split div/mul evita overflow do *1e6 a 10MHz.
                        LARGE_INTEGER qpc;
                        QueryPerformanceCounter(&qpc);
                        uint64_t q = (uint64_t)qpc.QuadPart;
                        uint64_t f = (uint64_t)qpc_freq_.QuadPart;
                        slot->ts_us = (q / f) * 1000000ull +
                                      (q % f) * 1000000ull / f;
                        slots_->publish_write(slot);
                    }
                    buf->Unlock();
                }
                buf->Release();
            }
        }
        (void)timestamp;
        // Re-arm the pump — async readers pull the next sample here.
        if (reader_)
            reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                0, nullptr, nullptr, nullptr, nullptr);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent *) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFlush(DWORD) override { return S_OK; }

    bool ended() const { return ended_.load(); }

private:
    std::atomic<ULONG> ref_;
    IMFSourceReader *reader_ = nullptr; // not owned
    FrameSlots *slots_ = nullptr;
    int w_ = 0, h_ = 0;
    uint32_t fmt_ = 0;
    uint64_t seq_ = 0;
    std::atomic<int> read_errors_{0};
    std::atomic<bool> ended_{false};
    LARGE_INTEGER qpc_freq_{ []() {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }() };
};

} // namespace

struct MfCapture::Impl {
    IMFMediaSource *source = nullptr;
    IMFSourceReader *reader = nullptr;
    ReaderCallback *cb = nullptr;
    int w = 0, h = 0, fps = 0;
    uint32_t fmt = 0;
    bool started = false;

    ~Impl() { close_all(); }

    void close_all()
    {
        if (reader) {
            // Flush drena leituras async em voo: o MF completa os
            // OnReadSample pendentes antes de soltarmos o reader e
            // o callback — nada fica apontando pra memoria morta.
            reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS);
            reader->Release();
            reader = nullptr;
        }
        if (cb) {
            cb->Release();
            cb = nullptr;
        }
        if (source) {
            source->Shutdown();
            source->Release();
            source = nullptr;
        }
    }
};

MfCapture::MfCapture() : p_(new Impl) {}
MfCapture::~MfCapture()
{
    close();
    delete p_;
}

std::vector<MfCameraInfo> MfCapture::list_devices()
{
    std::vector<MfCameraInfo> out;
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
        return out;

    IMFAttributes *attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1)))
        return out;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate **acts = nullptr;
    UINT32 n = 0;
    hr = MFEnumDeviceSources(attrs, &acts, &n);
    if (SUCCEEDED(hr)) {
        for (UINT32 i = 0; i < n; i++) {
            MfCameraInfo ci;
            ci.index = (int)i;
            LPWSTR name = nullptr, link = nullptr;
            if (SUCCEEDED(acts[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, nullptr)) && name)
                ci.name = name;
            if (SUCCEEDED(acts[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                    &link, nullptr)) && link)
                ci.symlink = link;
            out.push_back(ci);
            if (name) CoTaskMemFree(name);
            if (link) CoTaskMemFree(link);
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }
    attrs->Release();
    MFShutdown();
    return out;
}

void MfCapture::open(const MfOpenOptions &opts)
{
    check_hr(MFStartup(MF_VERSION), "MFStartup");

    IMFAttributes *attrs = nullptr;
    check_hr(MFCreateAttributes(&attrs, 2), "MFCreateAttributes");
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate **acts = nullptr;
    UINT32 n = 0;
    check_hr(MFEnumDeviceSources(attrs, &acts, &n), "MFEnumDeviceSources");
    attrs->Release();
    if (!n) {
        CoTaskMemFree(acts);
        throw std::runtime_error("nenhuma camera UVC encontrada");
    }
    if (opts.device_index < 0 || opts.device_index >= (int)n) {
        for (UINT32 i = 0; i < n; i++) acts[i]->Release();
        CoTaskMemFree(acts);
        throw std::runtime_error("indice de camera fora do intervalo");
    }
    check_hr(acts[opts.device_index]->ActivateObject(
               IID_IMFMediaSource, (void **)&p_->source),
           "ActivateObject(IMFMediaSource)");
    for (UINT32 i = 0; i < n; i++)
        if ((int)i != opts.device_index)
            acts[i]->Release();
    CoTaskMemFree(acts);

    // --- Optional camera locks: exposure / focus --------------------
    // The MF devsource on many UVC drivers QIs to the DirectShow-style
    // IAM interfaces. If the driver doesn't expose them we warn and go
    // on — capture is never blocked by a missing control.
    if (opts.lock_exposure || opts.lock_focus) {
        IAMCameraControl *cc = nullptr;
        IAMVideoProcAmp *pa = nullptr;
        HRESULT hr1 = p_->source->QueryInterface(IID_IAMCameraControl, (void **)&cc);
        HRESULT hr2 = p_->source->QueryInterface(IID_IAMVideoProcAmp, (void **)&pa);
        if (opts.lock_exposure && cc) {
            long v = 0, f = 0;
            if (SUCCEEDED(cc->Get(CameraControl_Exposure, &v, &f)))
                cc->Set(CameraControl_Exposure, v, CameraControl_Flags_Manual);
            else
                fprintf(stderr, "hvcam: lock-exposure nao suportado pelo driver\n");
        }
        if (opts.lock_exposure && pa) {
            long v = 0, f = 0;
            if (SUCCEEDED(pa->Get(VideoProcAmp_Brightness, &v, &f)))
                pa->Set(VideoProcAmp_Brightness, v, VideoProcAmp_Flags_Manual);
        }
        if (opts.lock_focus && cc) {
            long v = 0, f = 0;
            if (SUCCEEDED(cc->Get(CameraControl_Focus, &v, &f)))
                cc->Set(CameraControl_Focus, v, CameraControl_Flags_Manual);
            else
                fprintf(stderr, "hvcam: lock-focus nao suportado pelo driver\n");
        }
        if (!cc && (opts.lock_exposure || opts.lock_focus))
            fprintf(stderr, "hvcam: driver sem IAMCameraControl; locks ignorados\n");
        if (cc) cc->Release();
        if (pa) pa->Release();
    }

    // --- Async source reader ----------------------------------------
    p_->cb = new ReaderCallback();
    IMFAttributes *ra = nullptr;
    check_hr(MFCreateAttributes(&ra, 1), "MFCreateAttributes(reader)");
    check_hr(ra->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, p_->cb),
             "SetUnknown(ASYNC_CALLBACK)");
    check_hr(MFCreateSourceReaderFromMediaSource(p_->source, ra, &p_->reader),
             "MFCreateSourceReaderFromMediaSource");
    ra->Release();

    // --- Negotiate ONCE: pick best native type ----------------------
    // Subtype nao suportado sai ANTES de pontuar; o primeiro tipo
    // suportado ja e candidato, entao qualquer score ruim ainda
    // produz um fallback utilizavel em vez de rejeitar a camera.
    IMFMediaType *best = nullptr;
    int best_score = INT_MIN;
    for (DWORD i = 0;; i++) {
        IMFMediaType *mt = nullptr;
        HRESULT hr = p_->reader->GetNativeMediaType(
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &mt);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr))
            break;
        GUID sub = GUID_NULL;
        UINT32 w = 0, h = 0, fn = 0, fd = 0;
        mt->GetGUID(MF_MT_SUBTYPE, &sub);
        if (!subtype_to_fmt(sub)) {
            mt->Release();
            continue;
        }
        MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(mt, MF_MT_FRAME_RATE, &fn, &fd);
        int sc = score_type(sub, w, h, fn, fd, opts);
        if (sc > best_score) {
            if (best) best->Release();
            best = mt;
            best_score = sc;
            p_->w = (int)w;
            p_->h = (int)h;
            p_->fps = fd ? (int)(fn / fd) : 0;
            p_->fmt = subtype_to_fmt(sub);
        } else {
            mt->Release();
        }
    }
    if (!best)
        throw std::runtime_error("camera sem tipo nativo utilizavel");
    check_hr(p_->reader->SetCurrentMediaType(
               (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, best),
             "SetCurrentMediaType");
    best->Release();
    fprintf(stderr, "hvcam: %dx%d@%dfps fmt=%u\n", p_->w, p_->h, p_->fps, p_->fmt);
}

void MfCapture::start(FrameSlots *slots)
{
    if (!p_->reader)
        throw std::runtime_error("open() antes de start()");
    p_->cb->attach(p_->reader, slots, p_->w, p_->h, p_->fmt);
    check_hr(p_->reader->ReadSample(
               (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
               nullptr, nullptr, nullptr, nullptr),
             "ReadSample (primeira chamada)");
    p_->started = true;
}

void MfCapture::close()
{
    if (p_) {
        p_->close_all();
        MFShutdown();
    }
}

int MfCapture::width() const { return p_->w; }
int MfCapture::height() const { return p_->h; }
uint32_t MfCapture::format() const { return p_->fmt; }
int MfCapture::fps() const { return p_->fps; }

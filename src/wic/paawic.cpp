// PaaWic.dll: Windows Imaging Component decoder for PAA/PAC textures.
// Gives Explorer thumbnails, the preview pane and the Windows photo viewers through WIC.
// Install: regsvr32 PaaWic.dll (elevated). Uninstall: regsvr32 /u PaaWic.dll.
#include <windows.h>
#include <olectl.h>
#include <shlobj.h>
#include <wincodec.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "paa.h"

namespace {

// {24580B30-5677-498F-AE4D-C8CD74D2B606}
const CLSID CLSID_PaaDecoder = {0x24580b30, 0x5677, 0x498f, {0xae, 0x4d, 0xc8, 0xcd, 0x74, 0xd2, 0xb6, 0x06}};
// {7F303658-D793-4532-A9CE-66AE64B8B975}
const GUID GUID_ContainerFormatPaa = {0x7f303658, 0xd793, 0x4532, {0xa9, 0xce, 0x66, 0xae, 0x64, 0xb8, 0xb9, 0x75}};
// {9D7E90DC-7771-4B0C-AE60-892F88AD00CA}
const GUID GUID_VendorPaaViewer = {0x9d7e90dc, 0x7771, 0x4b0c, {0xae, 0x60, 0x89, 0x2f, 0x88, 0xad, 0x00, 0xca}};

const wchar_t kFriendlyName[] = L"PAA Texture Decoder (Arma)";
const wchar_t kExtensions[] = L".paa,.pac";
const wchar_t* const kShellExtensions[] = {L".paa", L".pac"};

// Shell handlers that render any WIC-decodable image.
const wchar_t kThumbnailHandlerKey[] = L"ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";
const wchar_t kPhotoThumbnailProvider[] = L"{C7657C4A-9F68-40fa-A4DF-96BC08EB3551}";
const wchar_t kPreviewHandlerKey[] = L"ShellEx\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";
const wchar_t kPhotoPreviewHandler[] = L"{FFE2A43C-56B9-4bf5-9A79-CC6D4285608A}";

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

// Minimal IUnknown implementation shared by the COM objects below.
template <class Derived>
class ComBase {
public:
    ComBase() { ++g_objects; }
    virtual ~ComBase() { --g_objects; }

protected:
    ULONG addRef() { return ++refs_; }
    ULONG release() {
        ULONG r = --refs_;
        if (r == 0) delete static_cast<Derived*>(this);
        return r;
    }

private:
    std::atomic<ULONG> refs_{1};
};

// Decoded file shared by the decoder and its frame. Mipmaps are decoded on first use.
struct Texture {
    std::vector<uint8_t> data;
    paa::Info info;
    std::mutex lock;
    std::vector<paa::Image> mips;
    std::vector<bool> decoded;
    std::atomic<ULONG> refs{1};

    void addRef() { ++refs; }
    void release() {
        if (--refs == 0) delete this;
    }

    const paa::Image* mip(size_t i) {
        std::lock_guard<std::mutex> guard(lock);
        if (i >= info.mipmaps.size()) return nullptr;
        if (mips.empty()) {
            mips.resize(info.mipmaps.size());
            decoded.assign(info.mipmaps.size(), false);
        }
        if (!decoded[i]) {
            if (!paa::decode(data.data(), data.size(), info, i, mips[i])) return nullptr;
            decoded[i] = true;
        }
        return &mips[i];
    }
};

HRESULT readStream(IStream* stream, std::vector<uint8_t>& out) {
    if (!stream) return E_INVALIDARG;
    STATSTG st = {};
    HRESULT hr = stream->Stat(&st, STATFLAG_NONAME);
    if (FAILED(hr)) return hr;
    if (st.cbSize.QuadPart == 0 || st.cbSize.QuadPart > (1ULL << 31)) return WINCODEC_ERR_BADIMAGE;
    LARGE_INTEGER zero = {};
    hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr)) return hr;
    out.resize(size_t(st.cbSize.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        ULONG got = 0;
        hr = stream->Read(out.data() + done, ULONG(out.size() - done), &got);
        if (FAILED(hr)) return hr;
        if (got == 0) break;
        done += got;
    }
    if (done != out.size()) return WINCODEC_ERR_BADIMAGE;
    return S_OK;
}

HRESULT copyRect(const paa::Image& img, const WICRect* prc, UINT stride, UINT bufferSize, BYTE* buffer) {
    WICRect full = {0, 0, INT(img.width), INT(img.height)};
    const WICRect& r = prc ? *prc : full;
    if (r.X < 0 || r.Y < 0 || r.Width < 0 || r.Height < 0 || UINT(r.X + r.Width) > img.width ||
        UINT(r.Y + r.Height) > img.height)
        return E_INVALIDARG;
    const UINT rowBytes = UINT(r.Width) * 4;
    if (stride < rowBytes) return E_INVALIDARG;
    if (r.Height > 0 && UINT64(stride) * (r.Height - 1) + rowBytes > bufferSize) return WINCODEC_ERR_INSUFFICIENTBUFFER;
    for (INT y = 0; y < r.Height; ++y) {
        const uint8_t* src = &img.bgra[(size_t(r.Y + y) * img.width + r.X) * 4];
        std::memcpy(buffer + size_t(stride) * y, src, rowBytes);
    }
    return S_OK;
}

class Frame : public ComBase<Frame>, public IWICBitmapFrameDecode, public IWICBitmapSourceTransform {
public:
    explicit Frame(Texture* tex) : tex_(tex) { tex_->addRef(); }
    ~Frame() override { tex_->release(); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IWICBitmapSource || iid == IID_IWICBitmapFrameDecode)
            *out = static_cast<IWICBitmapFrameDecode*>(this);
        else if (iid == IID_IWICBitmapSourceTransform)
            *out = static_cast<IWICBitmapSourceTransform*>(this);
        else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        addRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return addRef(); }
    STDMETHODIMP_(ULONG) Release() override { return release(); }

    // IWICBitmapSource
    STDMETHODIMP GetSize(UINT* w, UINT* h) override {
        if (!w || !h) return E_INVALIDARG;
        *w = tex_->info.mipmaps[0].width;
        *h = tex_->info.mipmaps[0].height;
        return S_OK;
    }
    STDMETHODIMP GetPixelFormat(WICPixelFormatGUID* fmt) override {
        if (!fmt) return E_INVALIDARG;
        *fmt = GUID_WICPixelFormat32bppBGRA;
        return S_OK;
    }
    STDMETHODIMP GetResolution(double* x, double* y) override {
        if (!x || !y) return E_INVALIDARG;
        *x = *y = 96.0;
        return S_OK;
    }
    STDMETHODIMP CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    STDMETHODIMP CopyPixels(const WICRect* prc, UINT stride, UINT size, BYTE* buffer) override {
        if (!buffer) return E_INVALIDARG;
        const paa::Image* img = tex_->mip(0);
        return img ? copyRect(*img, prc, stride, size, buffer) : WINCODEC_ERR_BADIMAGE;
    }

    // IWICBitmapFrameDecode
    STDMETHODIMP GetMetadataQueryReader(IWICMetadataQueryReader**) override {
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetColorContexts(UINT, IWICColorContext**, UINT* actual) override {
        if (!actual) return E_INVALIDARG;
        *actual = 0;
        return S_OK;
    }
    STDMETHODIMP GetThumbnail(IWICBitmapSource**) override { return WINCODEC_ERR_CODECNOTHUMBNAIL; }

    // IWICBitmapSourceTransform: lets thumbnailers read a small mipmap instead of the full texture.
    STDMETHODIMP CopyPixels(const WICRect* prc, UINT w, UINT h, WICPixelFormatGUID* fmt,
                            WICBitmapTransformOptions transform, UINT stride, UINT size, BYTE* buffer) override {
        if (!buffer) return E_INVALIDARG;
        if (transform != WICBitmapTransformRotate0) return WINCODEC_ERR_UNSUPPORTEDOPERATION;
        if (fmt && *fmt != GUID_WICPixelFormat32bppBGRA) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        for (size_t i = 0; i < tex_->info.mipmaps.size(); ++i) {
            const paa::Mipmap& m = tex_->info.mipmaps[i];
            if (m.width == w && m.height == h) {
                const paa::Image* img = tex_->mip(i);
                return img ? copyRect(*img, prc, stride, size, buffer) : WINCODEC_ERR_BADIMAGE;
            }
        }
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetClosestSize(UINT* w, UINT* h) override {
        if (!w || !h) return E_INVALIDARG;
        // Smallest mipmap that still covers the request; the full size otherwise.
        const paa::Mipmap* best = &tex_->info.mipmaps[0];
        for (const paa::Mipmap& m : tex_->info.mipmaps)
            if (m.width >= *w && m.height >= *h) best = &m;
        *w = best->width;
        *h = best->height;
        return S_OK;
    }
    STDMETHODIMP GetClosestPixelFormat(WICPixelFormatGUID* fmt) override {
        if (!fmt) return E_INVALIDARG;
        *fmt = GUID_WICPixelFormat32bppBGRA;
        return S_OK;
    }
    STDMETHODIMP DoesSupportTransform(WICBitmapTransformOptions transform, BOOL* supported) override {
        if (!supported) return E_INVALIDARG;
        *supported = transform == WICBitmapTransformRotate0;
        return S_OK;
    }

private:
    Texture* tex_;
};

class Decoder : public ComBase<Decoder>, public IWICBitmapDecoder {
public:
    ~Decoder() override {
        if (tex_) tex_->release();
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IWICBitmapDecoder) {
            *out = static_cast<IWICBitmapDecoder*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return addRef(); }
    STDMETHODIMP_(ULONG) Release() override { return release(); }

    // IWICBitmapDecoder
    STDMETHODIMP QueryCapability(IStream* stream, DWORD* caps) override {
        if (!caps) return E_INVALIDARG;
        *caps = 0;
        std::vector<uint8_t> data;
        HRESULT hr = readStream(stream, data);
        if (FAILED(hr)) return hr;
        paa::Info info;
        if (paa::parse(data.data(), data.size(), info))
            *caps = WICBitmapDecoderCapabilityCanDecodeAllImages | WICBitmapDecoderCapabilityCanDecodeSomeImages;
        return S_OK;
    }
    STDMETHODIMP Initialize(IStream* stream, WICDecodeOptions) override {
        if (tex_) return WINCODEC_ERR_WRONGSTATE;
        Texture* tex = new (std::nothrow) Texture;
        if (!tex) return E_OUTOFMEMORY;
        HRESULT hr = readStream(stream, tex->data);
        if (SUCCEEDED(hr) && !paa::parse(tex->data.data(), tex->data.size(), tex->info)) hr = WINCODEC_ERR_BADHEADER;
        if (FAILED(hr)) {
            tex->release();
            return hr;
        }
        tex_ = tex;
        return S_OK;
    }
    STDMETHODIMP GetContainerFormat(GUID* fmt) override {
        if (!fmt) return E_INVALIDARG;
        *fmt = GUID_ContainerFormatPaa;
        return S_OK;
    }
    STDMETHODIMP GetDecoderInfo(IWICBitmapDecoderInfo** info) override {
        if (!info) return E_INVALIDARG;
        *info = nullptr;
        IWICImagingFactory* factory = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;
        IWICComponentInfo* component = nullptr;
        hr = factory->CreateComponentInfo(CLSID_PaaDecoder, &component);
        if (SUCCEEDED(hr)) {
            hr = component->QueryInterface(IID_PPV_ARGS(info));
            component->Release();
        }
        factory->Release();
        return hr;
    }
    STDMETHODIMP CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    STDMETHODIMP GetMetadataQueryReader(IWICMetadataQueryReader**) override {
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetPreview(IWICBitmapSource**) override { return WINCODEC_ERR_UNSUPPORTEDOPERATION; }
    STDMETHODIMP GetColorContexts(UINT, IWICColorContext**, UINT* actual) override {
        if (!actual) return E_INVALIDARG;
        *actual = 0;
        return S_OK;
    }
    STDMETHODIMP GetThumbnail(IWICBitmapSource**) override { return WINCODEC_ERR_CODECNOTHUMBNAIL; }
    STDMETHODIMP GetFrameCount(UINT* count) override {
        if (!count) return E_INVALIDARG;
        *count = tex_ ? 1 : 0;
        return tex_ ? S_OK : WINCODEC_ERR_NOTINITIALIZED;
    }
    STDMETHODIMP GetFrame(UINT index, IWICBitmapFrameDecode** frame) override {
        if (!frame) return E_INVALIDARG;
        *frame = nullptr;
        if (!tex_) return WINCODEC_ERR_NOTINITIALIZED;
        if (index != 0) return WINCODEC_ERR_FRAMEMISSING;
        Frame* f = new (std::nothrow) Frame(tex_);
        if (!f) return E_OUTOFMEMORY;
        *frame = f;
        return S_OK;
    }

private:
    Texture* tex_ = nullptr;
};

class ClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) {
            *out = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }  // static instance
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        Decoder* d = new (std::nothrow) Decoder;
        if (!d) return E_OUTOFMEMORY;
        HRESULT hr = d->QueryInterface(iid, out);
        d->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_locks;
        else --g_locks;
        return S_OK;
    }
};

ClassFactory g_factory;

// ---- Registration -------------------------------------------------------------------------

std::wstring guidString(const GUID& g) {
    wchar_t buf[40];
    StringFromGUID2(g, buf, 40);
    return buf;
}

bool setString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY key;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool setDword(HKEY root, const std::wstring& path, const wchar_t* name, DWORD value) {
    HKEY key;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool setBinary(HKEY root, const std::wstring& path, const wchar_t* name, const BYTE* data, DWORD size) {
    HKEY key;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(key, name, 0, REG_BINARY, data, size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

const wchar_t kClasses[] = L"SOFTWARE\\Classes\\";
const wchar_t kKindMap[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap";

HRESULT registerServer() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return SELFREG_E_CLASS;

    const HKEY hklm = HKEY_LOCAL_MACHINE;
    const std::wstring clsid = guidString(CLSID_PaaDecoder);
    const std::wstring cls = std::wstring(kClasses) + L"CLSID\\" + clsid;
    bool ok = true;

    ok &= setString(hklm, cls, nullptr, kFriendlyName);
    ok &= setString(hklm, cls, L"FriendlyName", kFriendlyName);
    ok &= setString(hklm, cls, L"Author", L"paa-viewer");
    ok &= setString(hklm, cls, L"Version", L"1.0.0.0");
    ok &= setString(hklm, cls, L"SpecVersion", L"1.0.0.0");
    ok &= setString(hklm, cls, L"Vendor", guidString(GUID_VendorPaaViewer));
    ok &= setString(hklm, cls, L"ContainerFormat", guidString(GUID_ContainerFormatPaa));
    ok &= setString(hklm, cls, L"FileExtensions", kExtensions);
    ok &= setString(hklm, cls, L"MimeTypes", L"image/x-paa");
    ok &= setDword(hklm, cls, L"SupportAnimation", 0);
    ok &= setDword(hklm, cls, L"SupportChromakey", 0);
    ok &= setDword(hklm, cls, L"SupportLossless", 1);
    ok &= setDword(hklm, cls, L"SupportMultiframe", 0);
    ok &= setDword(hklm, cls, L"ArbitrationPriority", 10);
    ok &= setString(hklm, cls + L"\\InprocServer32", nullptr, path);
    ok &= setString(hklm, cls + L"\\InprocServer32", L"ThreadingModel", L"Both");
    ok &= setString(hklm, cls + L"\\Formats\\" + guidString(GUID_WICPixelFormat32bppBGRA), nullptr, L"");

    // Content sniffing: every PAA type word followed by a "GGAT" tag.
    static const uint16_t types[] = {0xFF01, 0xFF02, 0xFF03, 0xFF04, 0xFF05, 0x4444, 0x1555, 0x8888, 0x8080};
    static const BYTE mask[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        const std::wstring pat = cls + L"\\Patterns\\" + std::to_wstring(i);
        BYTE bytes[6] = {BYTE(types[i] & 0xFF), BYTE(types[i] >> 8), 'G', 'G', 'A', 'T'};
        ok &= setDword(hklm, pat, L"Position", 0);
        ok &= setDword(hklm, pat, L"Length", 6);
        ok &= setBinary(hklm, pat, L"Pattern", bytes, 6);
        ok &= setBinary(hklm, pat, L"Mask", mask, 6);
    }

    // WIC decoder category.
    const std::wstring inst = std::wstring(kClasses) + L"CLSID\\" + guidString(CATID_WICBitmapDecoders) +
                              L"\\Instance\\" + clsid;
    ok &= setString(hklm, inst, L"CLSID", clsid);
    ok &= setString(hklm, inst, L"FriendlyName", kFriendlyName);

    // Shell: picture kind plus the system photo thumbnail and preview handlers, which use WIC.
    for (const wchar_t* ext : kShellExtensions) {
        const std::wstring sfa = std::wstring(kClasses) + L"SystemFileAssociations\\" + ext;
        ok &= setString(hklm, sfa + L"\\" + kThumbnailHandlerKey, nullptr, kPhotoThumbnailProvider);
        ok &= setString(hklm, sfa + L"\\" + kPreviewHandlerKey, nullptr, kPhotoPreviewHandler);
        ok &= setString(hklm, sfa, L"PerceivedType", L"image");
        ok &= setString(hklm, kKindMap, ext, L"picture");
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok ? S_OK : SELFREG_E_CLASS;
}

HRESULT unregisterServer() {
    const HKEY hklm = HKEY_LOCAL_MACHINE;
    const std::wstring clsid = guidString(CLSID_PaaDecoder);
    RegDeleteTreeW(hklm, (std::wstring(kClasses) + L"CLSID\\" + clsid).c_str());
    RegDeleteTreeW(hklm, (std::wstring(kClasses) + L"CLSID\\" + guidString(CATID_WICBitmapDecoders) +
                          L"\\Instance\\" + clsid).c_str());
    for (const wchar_t* ext : kShellExtensions) {
        const std::wstring sfa = std::wstring(kClasses) + L"SystemFileAssociations\\" + ext;
        RegDeleteTreeW(hklm, (sfa + L"\\" + kThumbnailHandlerKey).c_str());
        RegDeleteTreeW(hklm, (sfa + L"\\" + kPreviewHandlerKey).c_str());
        RegDeleteKeyValueW(hklm, sfa.c_str(), L"PerceivedType");
        RegDeleteKeyValueW(hklm, kKindMap, ext);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (clsid != CLSID_PaaDecoder) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(iid, out);
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() { return registerServer(); }

STDAPI DllUnregisterServer() { return unregisterServer(); }

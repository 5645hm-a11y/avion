#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <atomic>
namespace media::platform::windows {
// Stable, prepared COM identity with an application-owned reference counter.
// An MFT may retain input samples beyond ProcessOutput. Reuse only at count one.
// Native sample storage/attributes are created once; no allocation or deletion in Release.
class NativeSampleLease final : public IMFSample {
public:
    IMFSample* storage = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    bool Available() const noexcept { return references_.load(std::memory_order_acquire) == 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IMFSample) && id != __uuidof(IMFAttributes)) return E_NOINTERFACE;
        *out = static_cast<IMFSample*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return references_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override { return references_.fetch_sub(1, std::memory_order_release) - 1; }
    void Close() noexcept { if (storage) { storage->Release(); storage = nullptr; } if (buffer) { buffer->Release(); buffer = nullptr; } }
#define MEDIA_SAMPLE_FORWARD(name, signature, arguments) HRESULT STDMETHODCALLTYPE name signature override { return storage->name arguments; }
    MEDIA_SAMPLE_FORWARD(GetItem, (REFGUID k, PROPVARIANT* v), (k, v))
    MEDIA_SAMPLE_FORWARD(GetItemType, (REFGUID k, MF_ATTRIBUTE_TYPE* v), (k, v))
    MEDIA_SAMPLE_FORWARD(CompareItem, (REFGUID k, REFPROPVARIANT v, BOOL* b), (k, v, b))
    MEDIA_SAMPLE_FORWARD(Compare, (IMFAttributes* v, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* b), (v, t, b))
    MEDIA_SAMPLE_FORWARD(GetUINT32, (REFGUID k, UINT32* v), (k, v))
    MEDIA_SAMPLE_FORWARD(GetUINT64, (REFGUID k, UINT64* v), (k, v))
    MEDIA_SAMPLE_FORWARD(GetDouble, (REFGUID k, double* v), (k, v))
    MEDIA_SAMPLE_FORWARD(GetGUID, (REFGUID k, GUID* v), (k, v))
    MEDIA_SAMPLE_FORWARD(GetStringLength, (REFGUID k, UINT32* n), (k, n))
    MEDIA_SAMPLE_FORWARD(GetString, (REFGUID k, LPWSTR v, UINT32 n, UINT32* r), (k, v, n, r))
    MEDIA_SAMPLE_FORWARD(GetAllocatedString, (REFGUID k, LPWSTR* v, UINT32* n), (k, v, n))
    MEDIA_SAMPLE_FORWARD(GetBlobSize, (REFGUID k, UINT32* n), (k, n))
    MEDIA_SAMPLE_FORWARD(GetBlob, (REFGUID k, UINT8* v, UINT32 n, UINT32* r), (k, v, n, r))
    MEDIA_SAMPLE_FORWARD(GetAllocatedBlob, (REFGUID k, UINT8** v, UINT32* n), (k, v, n))
    MEDIA_SAMPLE_FORWARD(GetUnknown, (REFGUID k, REFIID id, void** v), (k, id, v))
    MEDIA_SAMPLE_FORWARD(SetItem, (REFGUID k, REFPROPVARIANT v), (k, v))
    MEDIA_SAMPLE_FORWARD(DeleteItem, (REFGUID k), (k))
    MEDIA_SAMPLE_FORWARD(DeleteAllItems, (), ())
    MEDIA_SAMPLE_FORWARD(SetUINT32, (REFGUID k, UINT32 v), (k, v))
    MEDIA_SAMPLE_FORWARD(SetUINT64, (REFGUID k, UINT64 v), (k, v))
    MEDIA_SAMPLE_FORWARD(SetDouble, (REFGUID k, double v), (k, v))
    MEDIA_SAMPLE_FORWARD(SetGUID, (REFGUID k, REFGUID v), (k, v))
    MEDIA_SAMPLE_FORWARD(SetString, (REFGUID k, LPCWSTR v), (k, v))
    MEDIA_SAMPLE_FORWARD(SetBlob, (REFGUID k, const UINT8* v, UINT32 n), (k, v, n))
    MEDIA_SAMPLE_FORWARD(SetUnknown, (REFGUID k, IUnknown* v), (k, v))
    MEDIA_SAMPLE_FORWARD(LockStore, (), ())
    MEDIA_SAMPLE_FORWARD(UnlockStore, (), ())
    MEDIA_SAMPLE_FORWARD(GetCount, (UINT32* n), (n))
    MEDIA_SAMPLE_FORWARD(GetItemByIndex, (UINT32 n, GUID* k, PROPVARIANT* v), (n, k, v))
    MEDIA_SAMPLE_FORWARD(CopyAllItems, (IMFAttributes* v), (v))
    MEDIA_SAMPLE_FORWARD(GetSampleFlags, (DWORD* v), (v))
    MEDIA_SAMPLE_FORWARD(SetSampleFlags, (DWORD v), (v))
    MEDIA_SAMPLE_FORWARD(GetSampleTime, (LONGLONG* v), (v))
    MEDIA_SAMPLE_FORWARD(SetSampleTime, (LONGLONG v), (v))
    MEDIA_SAMPLE_FORWARD(GetSampleDuration, (LONGLONG* v), (v))
    MEDIA_SAMPLE_FORWARD(SetSampleDuration, (LONGLONG v), (v))
    MEDIA_SAMPLE_FORWARD(GetBufferCount, (DWORD* v), (v))
    MEDIA_SAMPLE_FORWARD(GetBufferByIndex, (DWORD n, IMFMediaBuffer** v), (n, v))
    MEDIA_SAMPLE_FORWARD(ConvertToContiguousBuffer, (IMFMediaBuffer** v), (v))
    MEDIA_SAMPLE_FORWARD(AddBuffer, (IMFMediaBuffer* v), (v))
    MEDIA_SAMPLE_FORWARD(RemoveBufferByIndex, (DWORD n), (n))
    MEDIA_SAMPLE_FORWARD(RemoveAllBuffers, (), ())
    MEDIA_SAMPLE_FORWARD(GetTotalLength, (DWORD* v), (v))
    MEDIA_SAMPLE_FORWARD(CopyToBuffer, (IMFMediaBuffer* v), (v))
#undef MEDIA_SAMPLE_FORWARD
private:
    std::atomic<ULONG> references_{1};
};
}

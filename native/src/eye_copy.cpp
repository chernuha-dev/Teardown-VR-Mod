// SPDX-License-Identifier: MIT
#include "tdvr_eye_copy.h"
#include <wrl/client.h>
#include <utility>

namespace tdvr {
namespace {
using Microsoft::WRL::ComPtr;
bool color_format(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        || format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
bool texture(const D3D12_RESOURCE_DESC& desc, bool typeless = false) {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width && desc.Height
        && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1
        && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
        && (color_format(desc.Format) || (typeless && (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS)));
}
bool state_ok(D3D12_RESOURCE_STATES state, const D3D12_RESOURCE_DESC& desc, bool source) {
    if (state == D3D12_RESOURCE_STATE_COMMON) return true;
    if (state == D3D12_RESOURCE_STATE_RENDER_TARGET) return (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    if (source && state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    return state == (source ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST);
}
}
static HRESULT record_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_before, ID3D12Resource* destination,
    D3D12_RESOURCE_STATES destination_before, DXGI_FORMAT display_view) noexcept {
    const bool display_srgb = display_view != DXGI_FORMAT_UNKNOWN;
    if (!list || !source || !destination || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    ComPtr<IUnknown> a, b, device_identity, other_identity;
    ComPtr<ID3D12Device> device, source_device, destination_device;
    HRESULT hr = source->QueryInterface(IID_PPV_ARGS(&a)); if (FAILED(hr)) return hr;
    hr = destination->QueryInterface(IID_PPV_ARGS(&b)); if (FAILED(hr)) return hr;
    if (a.Get() == b.Get()) return E_INVALIDARG;
    hr = list->GetDevice(IID_PPV_ARGS(&device)); if (FAILED(hr)) return hr;
    hr = device.As(&device_identity); if (FAILED(hr)) return hr;
    hr = source->GetDevice(IID_PPV_ARGS(&source_device)); if (FAILED(hr)) return hr;
    hr = source_device.As(&other_identity); if (FAILED(hr)) return hr;
    if (other_identity.Get() != device_identity.Get()) return E_INVALIDARG;
    hr = destination->GetDevice(IID_PPV_ARGS(&destination_device)); if (FAILED(hr)) return hr;
    hr = destination_device.As(&other_identity); if (FAILED(hr)) return hr;
    if (other_identity.Get() != device_identity.Get()) return E_INVALIDARG;
    const auto src = source->GetDesc(), dst = destination->GetDesc();
    const bool display_formats = (display_view == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        && (dst.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || dst.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || dst.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
        && (src.Format == DXGI_FORMAT_R8G8B8A8_UNORM || src.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
        || (display_view == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
        && (dst.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || dst.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || dst.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
        && (src.Format == DXGI_FORMAT_B8G8R8A8_UNORM || src.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB));
    if (!texture(src) || !texture(dst, display_srgb) || src.Width != dst.Width || src.Height != dst.Height
        || (display_srgb ? !display_formats : src.Format != dst.Format)
        || !state_ok(source_before, src, true) || !state_ok(destination_before, dst, false)) return E_INVALIDARG;
    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT count = 0;
    const auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        auto& barrier = barriers[count++]; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    };
    transition(source, source_before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(destination, destination_before, D3D12_RESOURCE_STATE_COPY_DEST);
    if (count) list->ResourceBarrier(count, barriers);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = source; to.pResource = destination;
    from.Type = to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    for (UINT i = 0; i < count; ++i) std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
    if (count) list->ResourceBarrier(count, barriers);
    return S_OK;
}
HRESULT record_eye_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_before, ID3D12Resource* destination,
    D3D12_RESOURCE_STATES destination_before) noexcept {
    return record_copy(list, source, source_before, destination, destination_before, DXGI_FORMAT_UNKNOWN);
}
HRESULT record_sdr_display_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_before, ID3D12Resource* destination,
    D3D12_RESOURCE_STATES destination_before, DXGI_COLOR_SPACE_TYPE color_space, DXGI_FORMAT declared_srgb_format) noexcept {
    if (color_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) return E_INVALIDARG;
    if (declared_srgb_format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && declared_srgb_format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return E_INVALIDARG;
    return record_copy(list, source, source_before, destination, destination_before, declared_srgb_format);
}
}

// SPDX-License-Identifier: MIT
// Real WARP allocations/clears/readback; no game or OpenXR.
#include "tdvr_eye_history.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
using Microsoft::WRL::ComPtr;
namespace {
void require(bool yes, const char* text) { if (!yes) { std::cerr << text << '\n'; std::exit(1); } }
void check(HRESULT result) { require(SUCCEEDED(result),"D3D12 failure"); }
void finish(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    const auto event = CreateEventW(nullptr,FALSE,FALSE,nullptr); require(event != nullptr,"fence event");
    check(queue->Signal(fence.Get(),1)); check(fence->SetEventOnCompletion(1,event));
    require(WaitForSingleObject(event,5000) == WAIT_OBJECT_0 && fence->GetCompletedValue() == 1,"bounded GPU completion"); CloseHandle(event);
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource,0,before,after}; list->ResourceBarrier(1,&barrier);
}
}
int main() {
    ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    tdvr::EyeHistoryResources bank(queue.Get()); tdvr::EyeHistoryView view;
    require(bank.begin_use(&view) == E_INVALIDARG && bank.seal_use() == E_UNEXPECTED,"uninitialized bank usable");
    D3D12_RESOURCE_DESC descriptions[2]{};
    for (auto& d : descriptions) {
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = 19; d.Height = 11;
        d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    descriptions[1].Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    require(bank.rebuild(nullptr,2) == E_INVALIDARG && bank.rebuild(descriptions,11) == E_INVALIDARG,"invalid bank bounds accepted");
    check(bank.rebuild(descriptions,2)); check(bank.begin_use(&view));
    require(view.count == 2 && bank.rebuild(descriptions,2) == E_PENDING && bank.begin_use(nullptr) == E_PENDING,"open use not protected");
    for (unsigned eye = 0; eye < 2; ++eye) for (unsigned i = 0; i < 2; ++i) {
        require(view.textures[eye][i] != view.textures[1-eye][i]
            && view.textures[eye][i]->GetDesc().Format == descriptions[i].Format,"eye allocations alias or lost precision");
    }
    ComPtr<ID3D12DescriptorHeap> heap; D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 4;
    check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list; check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
    std::array<ComPtr<ID3D12Resource>,4> readbacks;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[4]{}; UINT64 sizes[4]{};
    for (UINT eye = 0; eye < 2; ++eye) for (UINT i = 0; i < 2; ++i) {
        const UINT index = eye*2+i; auto* texture = view.textures[eye][i];
        auto rtv = heap->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += index*stride;
        device->CreateRenderTargetView(texture,nullptr,rtv);
        transition(list.Get(),texture,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float color[] = {eye ? 0.f : 1.f,eye ? 1.f : 0.f,0,1}; list->ClearRenderTargetView(rtv,color,0,nullptr);
        transition(list.Get(),texture,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        device->GetCopyableFootprints(&descriptions[i],0,1,0,&layouts[index],nullptr,nullptr,&sizes[index]);
        D3D12_HEAP_PROPERTIES read{}; read.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = sizes[index];
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&read,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readbacks[index])));
        D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource = texture; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readbacks[index].Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = layouts[index];
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        transition(list.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    }
    check(list->Close());
    ComPtr<ID3D12Fence> gate; check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
    check(queue->Wait(gate.Get(),1)); ID3D12CommandList* lists[] = {list.Get()}; queue->ExecuteCommandLists(1,lists);
    check(bank.seal_use());
    const auto saved = view;
    require(bank.rebuild(descriptions,2) == DXGI_ERROR_WAS_STILL_DRAWING
        && bank.begin_use(&view) == DXGI_ERROR_WAS_STILL_DRAWING && view.count == saved.count
        && !std::memcmp(view.textures,saved.textures,sizeof(view.textures))
        && !bank.quarantined(),"in-flight bank replaced or use output changed");
    check(gate->Signal(1)); finish(device.Get(),queue.Get());
    for (UINT eye = 0; eye < 2; ++eye) for (UINT i = 0; i < 2; ++i) {
        const UINT index = eye*2+i; void* bytes = nullptr; D3D12_RANGE range{0,static_cast<SIZE_T>(sizes[index])};
        check(readbacks[index]->Map(0,&range,&bytes));
        for (UINT y = 0; y < 11; ++y) for (UINT x = 0; x < 19; ++x) {
            const auto* pixel = static_cast<const uint8_t*>(bytes)+layouts[index].Offset+y*layouts[index].Footprint.RowPitch+x*(i ? 8 : 4);
            if (!i) require(pixel[0] == (eye ? 0 : 255) && pixel[1] == (eye ? 255 : 0) && pixel[2] == 0 && pixel[3] == 255,"eye history UNORM pixels mixed");
            else { uint16_t half[4]{}; std::memcpy(half,pixel,8);
                require(half[0] == (eye ? 0 : 0x3c00) && half[1] == (eye ? 0x3c00 : 0) && half[2] == 0 && half[3] == 0x3c00,"float16 history precision/eyes lost"); }
        }
        D3D12_RANGE empty{}; readbacks[index]->Unmap(0,&empty);
    }
    auto invalid = descriptions[0]; invalid.Width = 0;
    require(bank.rebuild(&invalid,1) == E_INVALIDARG,"invalid rebuild accepted");
    D3D12_RESOURCE_DESC failed[] = {descriptions[0],descriptions[1]}; failed[1].Format = DXGI_FORMAT_FORCE_UINT;
    require(FAILED(bank.rebuild(failed,2)),"unsupported allocation accepted");
    check(bank.begin_use(&view)); require(view.textures[0][0] == saved.textures[0][0],"failed rebuild lost previous bank");
    check(bank.seal_use()); finish(device.Get(),queue.Get());
    descriptions[0].Width = 23; check(bank.rebuild(descriptions,2)); check(bank.begin_use(&view));
    require(view.textures[0][0]->GetDesc().Width == 23 && view.textures[0][0] != saved.textures[0][0],"completed bank did not rebuild atomically");
    check(bank.seal_use()); finish(device.Get(),queue.Get());
    std::cout << "Independent UNORM/float16 eye histories, all 836 pixels and real pending-fence replacement gate verified\n";
}

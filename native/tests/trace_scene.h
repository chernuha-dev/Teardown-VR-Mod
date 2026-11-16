#pragma once
// SPDX-License-Identifier: MIT
// Actual GPU workload for the recording hooks; no game/runtime dependencies.

struct TraceScene {
    ComPtr<ID3D12RootSignature> graphics_root, compute_root;
    ComPtr<ID3D12PipelineState> graphics, compute;
    ComPtr<ID3D12Resource> indices;
    explicit TraceScene(ID3D12Device* device) {
        auto compile = [](const char* text, const char* target) {
            ComPtr<ID3DBlob> shader, error;
            check(D3DCompile(text, std::strlen(text), nullptr, nullptr, nullptr, "main", target,
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &error), "compile trace shader");
            return shader;
        };
        auto root = [&](D3D12_ROOT_SIGNATURE_DESC desc) {
            ComPtr<ID3DBlob> blob, error;
            check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "serialize trace root");
            ComPtr<ID3D12RootSignature> result;
            check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&result)), "trace root");
            return result;
        };
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants.Num32BitValues = 4;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 1; rd.pParameters = &parameter;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        graphics_root = root(rd);
        compute_root = root(D3D12_ROOT_SIGNATURE_DESC{});
        auto vs = compile("float4 main(uint id:SV_VertexID):SV_Position {float2 uv=float2((id<<1)&2,id&2); return float4(uv*float2(2,-2)+float2(-1,1),0,1);}", "vs_5_0");
        auto ps = compile("cbuffer Color:register(b0){float4 tint;} float4 main():SV_Target{return tint;}", "ps_5_0");
        auto cs = compile("[numthreads(1,1,1)] void main(){}", "cs_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gd{};
        gd.pRootSignature = graphics_root.Get();
        gd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        gd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gd.RasterizerState.DepthClipEnable = TRUE;
        auto& blend = gd.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        gd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        gd.DepthStencilState.FrontFace = gd.DepthStencilState.BackFace =
            {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        gd.SampleMask = ~UINT{0};
        gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gd.NumRenderTargets = 1; gd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gd.SampleDesc.Count = 1;
        check(device->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&graphics)), "trace graphics pipeline");
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
        cd.pRootSignature = compute_root.Get(); cd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        check(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&compute)), "trace compute pipeline");
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = 12;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&indices)), "trace index buffer");
        void* data = nullptr; D3D12_RANGE no_reads{};
        check(indices->Map(0, &no_reads, &data), "map trace index buffer");
        const uint32_t values[]{0,1,2}; std::memcpy(data, values, sizeof(values)); indices->Unmap(0, nullptr);
    }
    void render(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        UINT width, UINT height, const float* color) {
        list->SetComputeRootSignature(compute_root.Get()); list->SetPipelineState(compute.Get());
        list->Dispatch(2,3,1);
        const float black[]{0,0,0,0}; list->ClearRenderTargetView(rtv, black, 0, nullptr);
        list->SetGraphicsRootSignature(graphics_root.Get()); list->SetPipelineState(graphics.Get());
        list->SetGraphicsRoot32BitConstants(0, 4, color, 0);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};
        list->RSSetViewports(1, &viewport); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const LONG middle = static_cast<LONG>(width/2);
        const D3D12_RECT left{0,0,middle,static_cast<LONG>(height)};
        list->RSSetScissorRects(1, &left); list->DrawInstanced(3,1,0,0);
        const D3D12_RECT right{middle,0,static_cast<LONG>(width),static_cast<LONG>(height)};
        const D3D12_INDEX_BUFFER_VIEW ib{indices->GetGPUVirtualAddress(),12,DXGI_FORMAT_R32_UINT};
        list->IASetIndexBuffer(&ib); list->RSSetScissorRects(1, &right);
        list->DrawIndexedInstanced(3,1,0,0,0);
    }
};

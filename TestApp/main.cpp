#define WIN32_LEAN_AND_MEAN // Exclude rarely used stuff from Windows headers
#define NOMINMAX
#include <Windows.h>
#include <crtdbg.h>
#include "../Core/Log.h"
#include "../Core/Window.h"
#include "../Device/Device.h"
#include "../Device/CommandGraph.h"
#include "../Resource/RenderTarget.h"
#include "../Resource/CommitedResource.h"
#include "../Resource/Mesh.h"
#include <iostream>
#include "pix3.h"

using namespace FrameDX12;
using namespace std;
using namespace fpp;

constexpr int kWorkerCount = 4;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance, PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    // -------------------------------
    //      General setup
    // -------------------------------

    // Init the log
    Log.CreateConsole();
    auto print_thread = Log.FirePrintThread();

    // Create the window
    Window window;

    // Create the device
    Device dev(&window);

    // -------------------------------
    //      Specific setup
    // -------------------------------

    // Create the resources
    RenderTarget backbuffer;
    backbuffer.CreateFromSwapchain(&dev);

    CommitedResource depth_buffer;
    {
        CD3DX12_RESOURCE_DESC depth_desc(
            D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            0,
            static_cast<UINT>(window.GetSizeX()),
            static_cast<UINT>(window.GetSizeY()),
            1,
            1,
            DXGI_FORMAT_D32_FLOAT,
            1,
            0,
            D3D12_TEXTURE_LAYOUT_UNKNOWN,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);

        D3D12_CLEAR_VALUE clear_value;	// Performance tip: Tell the runtime at resource creation the desired clear value.
        clear_value.Format = DXGI_FORMAT_D32_FLOAT;
        clear_value.DepthStencil.Depth = 1.0f;
        clear_value.DepthStencil.Stencil = 0;

        depth_buffer.Create(&dev, depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value);
        depth_buffer.CreateDSV();
    }

    //  Create root signature
    ComPtr<ID3D12RootSignature> root_signature;
    {
        CD3DX12_DESCRIPTOR_RANGE ranges[1]; // Perfomance TIP: Order from most frequent to least frequent.
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);		// 1 frequently changed constant buffer.

        CD3DX12_ROOT_PARAMETER rootParameters[1];
        rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init(_countof(rootParameters), rootParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
        ThrowIfFailed(dev.GetDevice()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root_signature)));
    }

    // Load shaders
    ComPtr<ID3DBlob> vertex_shader;
    ComPtr<ID3DBlob> pixel_shader;

#ifdef _DEBUG
    // Enable better shader debugging with the graphics debugging tools.
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    // TODO : Handle failure
    ComPtr<ID3DBlob> error_blob;
    LogCheck(D3DCompileFromFile(L"SimpleShaders.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertex_shader, &error_blob), LogCategory::Error);
    LogErrorBlob(error_blob);

    LogCheck(D3DCompileFromFile(L"SimpleShaders.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixel_shader, &error_blob), LogCategory::Error);
    LogErrorBlob(error_blob);

    // Define pipeline state 
    D3D12_INPUT_LAYOUT_DESC input_layout;
    input_layout.pInputElementDescs = StandardVertex::sDesc;
    input_layout.NumElements = _countof(StandardVertex::sDesc);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_state = {};
    pipeline_state.InputLayout = input_layout;
    pipeline_state.pRootSignature = root_signature.Get();
    pipeline_state.VS = { reinterpret_cast<UINT8*>(vertex_shader->GetBufferPointer()), vertex_shader->GetBufferSize() };
    pipeline_state.PS = { reinterpret_cast<UINT8*>(pixel_shader->GetBufferPointer()), pixel_shader->GetBufferSize() };
    pipeline_state.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pipeline_state.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pipeline_state.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pipeline_state.SampleMask = UINT_MAX;
    pipeline_state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline_state.NumRenderTargets = 1;
    pipeline_state.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipeline_state.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pipeline_state.SampleDesc.Count = 1;

    // Load mesh
    CommandGraph copy_graph(kWorkerCount, QueueType::Copy, &dev);
    
    Mesh monkey;
    monkey.BuildFromOBJ(&dev, copy_graph, "monkey.obj");
    vector<Mesh> monkeys(100);
    for (auto& m : monkeys) m = monkey;

    copy_graph.Build(&dev);
    copy_graph.Execute(&dev);

    // -------------------------------
    //      Render setup
    // -------------------------------

    // Create the command graph
    CommandGraph commands(kWorkerCount, QueueType::Graphics, &dev);

    commands.AddNode("Clear", nullptr, [&](ID3D12GraphicsCommandList* cl, uint32_t)
    {
        backbuffer.Transition(cl, D3D12_RESOURCE_STATE_RENDER_TARGET);

        cl->ClearRenderTargetView(*backbuffer.GetHandle(), DirectX::Colors::Magenta, 0, nullptr);
        cl->ClearDepthStencilView(*depth_buffer.GetDSV(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }, {});

    // TODO : Add functions on the window to return this structs
    D3D12_VIEWPORT viewport = {};
    viewport.Width = window.GetSizeX();
    viewport.Height = window.GetSizeY();
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissor_rect = {};
    scissor_rect.right = static_cast<LONG>(viewport.Width);
    scissor_rect.bottom = static_cast<LONG>(viewport.Height);

    commands.AddNode("Draw", [&](ID3D12GraphicsCommandList* cl)
    {
        // While this state is shared, and could be set earlier, doing execute command lists seems to clear it
        cl->RSSetViewports(1, &viewport);
        cl->RSSetScissorRects(1, &scissor_rect);
        cl->SetGraphicsRootSignature(root_signature.Get());
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->OMSetRenderTargets(1, &*backbuffer.GetHandle(), false, &*depth_buffer.GetDSV()); 
    },
    [&](ID3D12GraphicsCommandList* cl, uint32_t idx)
    {
        monkeys[idx].Draw(cl);
    }, {"Clear"}, monkeys.size());

    commands.AddNode("Present", nullptr, [&](ID3D12GraphicsCommandList* cl, uint32_t)
    {
        backbuffer.Transition(cl, D3D12_RESOURCE_STATE_PRESENT);
    }, {"Draw"});

    commands.Build(&dev);

    // -------------------------------
    //      Render loop
    // -------------------------------
    // Make sure all transfers finished
    dev.WaitForQueue(QueueType::Copy);

    // To print some debug metrics
    float execute_cl_time, frame_time;
    thread metrics_printer([&]()
    {
        FrameDX12::TimedLoop([&]()
        {
            system("cls");
            wcout << L"---- Metrics ----" << endl;
            wcout << L"Execute CL : " << to_wstring(execute_cl_time) << endl;
            wcout << L"Frame      : " << to_wstring(frame_time) << endl;
        }, 150ms);
    });
    metrics_printer.detach();

    // Enter the render loop
    window.CallDuringIdle([&](double elapsed_time)
    {
        frame_time = elapsed_time;

        auto start = chrono::high_resolution_clock::now();
            commands.Execute(&dev, dev.GetPSO(pipeline_state));
        auto end = chrono::high_resolution_clock::now();
        execute_cl_time = chrono::duration_cast<chrono::nanoseconds>((end - start)).count() / 1e6;

        dev.GetSwapChain()->Present(0, 0);

        // Advance buffer index
        sCurrentResourceBufferIndex++;

        // If we are going to roll back to the first allocator, then we need to wait
        if (sCurrentResourceBufferIndex % kResourceBufferCount == 0)
        {
            dev.WaitForQueue(QueueType::Graphics);
            // Add the other queues if you are doing work there
        }

        return false;
    });

    return 0;
}
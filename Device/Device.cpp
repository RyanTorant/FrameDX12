#include "Device.h"
#include "../Core/Log.h"
#include <iostream>
#include "../Resource/BufferedResource.h"
#include "../Core/Window.h"

using namespace FrameDX12;

Device::Device(Window* window_ptr, int adapter_index) 
	: mPSOPool(this)
{
	using namespace std;

#if defined(DEBUG) || defined(_DEBUG) 
	// Enable the D3D12 debug layer.
	{
		ComPtr<ID3D12Debug> debug_controller;
		ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)));
		debug_controller->EnableDebugLayer();

		// This may cause PIX to crash
		ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred_settings;
		ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&dred_settings)));

		// Turn on auto-breadcrumbs and page fault reporting.
		dred_settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	}
#endif

	// Find the graphics adapter
	IDXGIAdapter* adapter = nullptr;
	IDXGIFactory* factory = nullptr;
	bool using_factory2 = true;

	UINT dxgi2_flags = 0;
#ifdef _DEBUG
	dxgi2_flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
	if (CreateDXGIFactory2(dxgi2_flags, IID_PPV_ARGS(&factory)) != S_OK)
	{
		using_factory2 = false;
		ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
	}

	ComPtr<IDXGIFactory6> factory6;
	const bool bCanSortAdapters = (factory->QueryInterface(IID_PPV_ARGS(&factory6)) == S_OK);
	if (adapter_index == -1)
	{
		for (UINT i = 0; 
			(bCanSortAdapters ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) : 
				factory->EnumAdapters(i, &adapter)) != DXGI_ERROR_NOT_FOUND; 
			i++)
		{
			DXGI_ADAPTER_DESC desc;
			adapter->GetDesc(&desc);

			wcout << L"GPU : " << i << L" , " << desc.Description << endl;
		}

		wcout << L"Select GPU : ";
		cin >> adapter_index;
	}

	if (bCanSortAdapters)
	{
		factory6->EnumAdapterByGpuPreference(adapter_index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
	}
	else
	{
		factory->EnumAdapters(adapter_index, &adapter);
	}

	// Create the device
	// Try first with feature level 12.1
	D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_12_1;
	ThrowIfFailed(D3D12CreateDevice(adapter, level, IID_PPV_ARGS(&mD3DDevice)));
	if (!mD3DDevice)
	{
		// If the device failed to be created, try with feature level 12.0
		LogMsg(L"Failed to create device with feature level 12.1, trying with feature level 12.0", LogCategory::Info);

		level = D3D_FEATURE_LEVEL_12_0;
		ThrowIfFailed(D3D12CreateDevice(adapter, level, IID_PPV_ARGS(&mD3DDevice)));
	}

	// Check if the device can be casted to anything higher than 11.0
	ID3D12Device* new_device;
	mDeviceVersion = 0;
	if (mD3DDevice->QueryInterface(__uuidof(ID3D12Device5), (void**)&new_device) == S_OK)
	{
		mD3DDevice = new_device;
		mDeviceVersion = 5;
	}
	else if (mD3DDevice->QueryInterface(__uuidof(ID3D12Device4), (void**)&new_device) == S_OK)
	{
		mD3DDevice = new_device;
		mDeviceVersion = 4;
	}
	else if (mD3DDevice->QueryInterface(__uuidof(ID3D12Device3), (void**)&new_device) == S_OK)
	{
		mD3DDevice = new_device;
		mDeviceVersion = 3;
	}
	else if (mD3DDevice->QueryInterface(__uuidof(ID3D12Device2), (void**)&new_device) == S_OK)
	{
		mD3DDevice = new_device;
		mDeviceVersion = 2;
	}
	else if (mD3DDevice->QueryInterface(__uuidof(ID3D12Device1), (void**)&new_device) == S_OK)
	{
		mD3DDevice = new_device;
		mDeviceVersion = 1;
	}

	LogMsg(wstring(L"Created a device of version ") + to_wstring(mDeviceVersion), LogCategory::Info);

	// Create the command queues
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	ThrowIfFailed(mD3DDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mGraphicsQueue)));

	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
	ThrowIfFailed(mD3DDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mComputeQueue)));

	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	ThrowIfFailed(mD3DDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCopyQueue)));

	// Create fences
	for (auto& [fence, last_work_id, sync_event] : mFences)
	{
		last_work_id = 0;
		ThrowIfFailed(mD3DDevice->CreateFence(last_work_id, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
		sync_event = CreateEventEx(nullptr, FALSE, FALSE, EVENT_ALL_ACCESS);
	}

	// Describe and create the swap chain.
	DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
	swapChainDesc.BufferCount = kResourceBufferCount;
	swapChainDesc.BufferDesc.Width = window_ptr->GetSizeX();
	swapChainDesc.BufferDesc.Height = window_ptr->GetSizeY();
	swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // TODO : Expose!
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.OutputWindow = window_ptr->GetHandle();
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.Windowed = !window_ptr->IsFullscreen();

	ThrowIfFailed(factory->CreateSwapChain(
		mGraphicsQueue.Get(),		// Swap chain needs the queue so that it can force a flush on it.
		&swapChainDesc,
		&mSwapChain
	));

	IDXGISwapChain* new_swapchain;
	mSwapChainVersion = 0;
	if (mSwapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&new_swapchain) == S_OK)
	{
		mSwapChain = new_swapchain;
		mDeviceVersion = 3;
	}
	else if (mSwapChain->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&new_swapchain) == S_OK)
	{
		mSwapChain = new_swapchain;
		mDeviceVersion = 2;
	}
	else if (mSwapChain->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&new_swapchain) == S_OK)
	{
		mSwapChain = new_swapchain;
		mDeviceVersion = 1;
	}

	// Create descriptor vectors
	{
		D3D12_DESCRIPTOR_HEAP_TYPE type;

		type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		mDescriptorPools[type].Initialize(this, type, true, 256);

		type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		mDescriptorPools[type].Initialize(this, type, false, 512);

		type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		mDescriptorPools[type].Initialize(this, type, false, 256);

		type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		mDescriptorPools[type].Initialize(this, type, true, 65536);
	}
}

uint64_t Device::SignalQueueWork(QueueType queue)
{
	auto& fence = mFences[QueueTypeToIndex(queue)];
	++fence.last_work_id;
	ThrowIfFailed(GetQueue(queue)->Signal(fence.fence.Get(), fence.last_work_id));
	return fence.last_work_id;
}

void Device::WaitForQueue(QueueType queue)
{
	auto& fence = mFences[QueueTypeToIndex(queue)];
	if (fence.fence->GetCompletedValue() < fence.last_work_id)
	{
		ThrowIfFailed(fence.fence->SetEventOnCompletion(fence.last_work_id, fence.sync_event));
		WaitForSingleObject(fence.sync_event, INFINITE);
	}
}

void Device::WaitForWork(QueueType queue, uint64_t id)
{
	auto& fence = mFences[QueueTypeToIndex(queue)];
	if (fence.fence->GetCompletedValue() < id)
	{
		ThrowIfFailed(fence.fence->SetEventOnCompletion(id, fence.sync_event));
		WaitForSingleObject(fence.sync_event, INFINITE);
	}
}
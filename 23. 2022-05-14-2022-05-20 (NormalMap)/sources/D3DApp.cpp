﻿#include "D3DApp.h"

namespace Mawi1e {
	template <class _Tp>
	_Tp& My_unmove(_Tp&& __value) {
		return __value;
	}

	D3DApp::D3DApp() {
		m_D3DApp = this;
	}

	D3DApp::D3DApp(const D3DApp&) {
	}

	D3DApp::~D3DApp() {
		Shutdown();
	}

	void D3DApp::Initialize(const D3DSettings& d3dSettings) {
		m_d3dSettings = d3dSettings;

#if defined(DEBUG) || defined(_DEBUG)
		EnableDebugLayer();
#endif

		THROWFAILEDIF("@@@ Error: CreateDXGIFactory1",
			CreateDXGIFactory1(IID_PPV_ARGS(m_Factory.GetAddressOf())));

		if (d3dSettings.debugMode) {
			InitializeConsole();

			LogAdapter();
			LogOutput();
			LogModeLists();
		}

		CreateDevice();
		Check4xMsaa();

		CreateFenceAndDescriptorSize();
		CreateCommandInterface();
		CreateSwapChain();
		CreateDescriptorHeap();

		ResizeBuffer();

		/* --------------------------------------------------------------------------------------
		[                               루트서명&그래픽파이프라인                               ]
		-------------------------------------------------------------------------------------- */

		m_CommandList->Reset(m_CommandAllocator.Get(), nullptr);

		m_Camera.SetPosition(0.0f, 2.0f, -15.0f);
		BuildCubeFaceCamera(10.0f, 0.0f, 0.0f);

		m_CubeRenderTarget = std::make_unique<CubeRenderTarget>(m_Device.Get(),
			gCubemapSize, gCubemapSize, m_BackBufferFormat);

		LoadTexture();
		BuildRootSignature();
		BuildDescriptorHeaps();
		BuildDynamicDepthStencil();
		BuildShadersAndInputlayout();
		BuildInstancesTheSkull();
		BuildShapeGeometry();
		BuildPlaneGeometry();
		BuildMaterials();
		BuildRenderItems();
		BuildFrameResources();
		BuildPSO();


		m_CommandList->Close();
		ID3D12CommandList* cmdList[] = { m_CommandList.Get() };
		m_CommandQueue->ExecuteCommandLists(_countof(cmdList), cmdList);

		FlushCommandQueue();
	}

	void D3DApp::Shutdown() {
		if (m_SwapChain != nullptr) {
			FlushCommandQueue();
		}
	}

	float D3DApp::AspectRatio() const {
		return static_cast<float>(m_d3dSettings.screenWidth) / m_d3dSettings.screenHeight;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::GetRtvHandle() {
		auto a = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_RtvHeap->GetCPUDescriptorHandleForHeapStart(), m_CurrBackBufferIdx, m_RtvSize);
		return a;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::GetDsvHandle() {
		return m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
	}

	void D3DApp::FlushCommandQueue() {
		++m_FenceCount;

		THROWFAILEDIF("@@@ Error: ID3D12CommandQueue::Signal",
			m_CommandQueue->Signal(m_Fence.Get(), m_FenceCount));

		if (m_Fence->GetCompletedValue() < m_FenceCount) {
			HANDLE hEvent = CreateEventEx(0, 0, 0, EVENT_ALL_ACCESS);

			THROWFAILEDIF("@@@ Error: ID3D12Fence::SetEventOnCompletion",
				m_Fence->SetEventOnCompletion(m_FenceCount, hEvent));

			WaitForSingleObject(hEvent, INFINITE);
			CloseHandle(hEvent);
		}
	}

	void D3DApp::Update(const GameTimer* gameTimer) {
		OnKeyboardInput(gameTimer);
		UpdateWindowTitle(gameTimer);
		UpdateSkullPosition(gameTimer->TotalTime());

		m_CurrFrameResourceIndex = (m_CurrFrameResourceIndex + 1) % gNumFrameResources;
		m_CurrFrameResource = m_FrameResources[m_CurrFrameResourceIndex].get();

		if (m_CurrFrameResource->m_Fence != 0 && m_Fence->GetCompletedValue() < m_CurrFrameResource->m_Fence) {
			HANDLE hEvent = CreateEventExW(nullptr, 0, 0, EVENT_ALL_ACCESS);
			m_Fence->SetEventOnCompletion(m_CurrFrameResource->m_Fence, hEvent);
			WaitForSingleObject(hEvent, INFINITE);
			CloseHandle(hEvent);
		}

		UpdateObjectCB(gameTimer);
		UpdateMatetialCBs(gameTimer);
		UpdatePassCB();
	}

	void D3DApp::Draw(const GameTimer* gameTimer) {
		auto cmdListAlloc = m_CurrFrameResource->m_CommandAllocator;
		cmdListAlloc->Reset();

		if (m_IsWireFrames) {
			m_CommandList->Reset(cmdListAlloc.Get(), m_PSOs["opaque_wireframe"].Get());
		}
		else {
			m_CommandList->Reset(cmdListAlloc.Get(), m_PSOs["opaque"].Get());
		}

		ID3D12DescriptorHeap* descriptorHeaps[] = { m_SrvDescriptorHeap.Get() };
		m_CommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

		m_CommandList->SetGraphicsRootSignature(m_RootSignature.Get());
		
		UINT passCBByteSize = VertexBuffer::CalcConstantBufferSize(sizeof(PassConstants));

		auto m = m_CurrFrameResource->m_MatVB->Resource();
		m_CommandList->SetGraphicsRootShaderResourceView(2, m->GetGPUVirtualAddress());
		CD3DX12_GPU_DESCRIPTOR_HANDLE skyHandle(m_SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		skyHandle.Offset(m_SnowCubeMapTextureIndex, m_CbvSize);
		m_CommandList->SetGraphicsRootDescriptorTable(3, skyHandle);
		m_CommandList->SetGraphicsRootDescriptorTable(4, m_SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

		DrawSceneToCubemap();

		m_CommandList->RSSetViewports(1, &m_ViewPort);
		m_CommandList->RSSetScissorRects(1, &m_ScissorRect);

		auto rtv = GetRtvHandle();
		auto dsv = GetDsvHandle();


		D3D12_RESOURCE_BARRIER resourceBarrier_1 =
			CD3DX12_RESOURCE_BARRIER::Transition(m_RtvDescriptor[m_CurrBackBufferIdx].Get(),
				D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		m_CommandList->ResourceBarrier(1, &resourceBarrier_1);

		m_CommandList->ClearRenderTargetView(rtv, (float*)&m_MainPassCB.FogColor, 0, nullptr);
		m_CommandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

		m_CommandList->OMSetRenderTargets(1, &rtv, true, &dsv);


		auto p = m_CurrFrameResource->m_PassCB->Resource();
		m_CommandList->SetGraphicsRootConstantBufferView(1, p->GetGPUVirtualAddress());

		CD3DX12_GPU_DESCRIPTOR_HANDLE cubemapHandle(m_SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		cubemapHandle.Offset(m_DynamicCubemapIndex, m_CbvSize);
		m_CommandList->SetGraphicsRootDescriptorTable(3, cubemapHandle);
		// dynamic cube
		DrawRenderItems(m_CommandList.Get(), mRitemLayer[(int)RenderLayer::DynamicCubemapOpaque]);
		m_CommandList->SetGraphicsRootDescriptorTable(3, skyHandle);

		// opaque
		DrawRenderItems(m_CommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

		// skull
		DrawRenderItems(m_CommandList.Get(), mRitemLayer[(int)RenderLayer::Skull]);

		// highlight
		m_CommandList->SetPipelineState(m_PSOs["highlight"].Get());
		DrawRenderItems(m_CommandList.Get(), mRitemLayer[(int)RenderLayer::Highlight]);

		// sky
		m_CommandList->SetPipelineState(m_PSOs["sky"].Get());
		DrawRenderItems(m_CommandList.Get(), mRitemLayer[(int)RenderLayer::Sky]);
		
		D3D12_RESOURCE_BARRIER resourceBarrier_2 =
			CD3DX12_RESOURCE_BARRIER::Transition(m_RtvDescriptor[m_CurrBackBufferIdx].Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		m_CommandList->ResourceBarrier(1, &resourceBarrier_2);


		m_CommandList->Close();

		ID3D12CommandList* commandlists[] = { m_CommandList.Get() };
		m_CommandQueue->ExecuteCommandLists(_countof(commandlists), commandlists);

		m_SwapChain->Present(0, 0);
		m_CurrBackBufferIdx = (m_CurrBackBufferIdx + 1) % m_BackBufferCount;

		m_CurrFrameResource->m_Fence = ++m_FenceCount;
		m_CommandQueue->Signal(m_Fence.Get(), m_FenceCount);
	}

	void D3DApp::ResizeBuffer() {
		FlushCommandQueue();

		m_CommandList->Reset(m_CommandAllocator.Get(), 0);

		for (UINT i = 0; i < m_BackBufferCount; ++i) {
			m_RtvDescriptor[i].Reset();
		}
		m_DsvDescriptor.Reset();

		m_SwapChain->ResizeBuffers(m_BackBufferCount,
			m_d3dSettings.screenWidth, m_d3dSettings.screenHeight,
			m_BackBufferFormat, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);

		m_CurrBackBufferIdx = 0;

		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_RtvHeap->GetCPUDescriptorHandleForHeapStart());
		for (UINT i = 0; i < m_BackBufferCount; ++i) {
			m_SwapChain->GetBuffer(i, IID_PPV_ARGS(m_RtvDescriptor[i].GetAddressOf()));
			m_Device->CreateRenderTargetView(m_RtvDescriptor[i].Get(), nullptr, rtvHandle);
			rtvHandle.Offset(1, m_RtvSize);
		}

		D3D12_RESOURCE_DESC dsvResourceDesc;
		dsvResourceDesc.Alignment = 0;
		dsvResourceDesc.DepthOrArraySize = 1;
		dsvResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		dsvResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		dsvResourceDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
		dsvResourceDesc.Height = m_d3dSettings.screenHeight;
		dsvResourceDesc.Width = m_d3dSettings.screenWidth;
		dsvResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		dsvResourceDesc.MipLevels = 1;
		dsvResourceDesc.SampleDesc.Count = m_MSQualityState ? 4 : 1;
		dsvResourceDesc.SampleDesc.Quality = m_MSQualityState ? (m_MultisampleQuality - 1) : 0;

		D3D12_CLEAR_VALUE clearValue;
		clearValue.Format = m_DepthStencilFormat;
		clearValue.DepthStencil.Depth = 1.0f;
		clearValue.DepthStencil.Stencil = 0;

		CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
		m_Device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
			&dsvResourceDesc, D3D12_RESOURCE_STATE_COMMON,
			&clearValue, IID_PPV_ARGS(m_DsvDescriptor.GetAddressOf()));

		D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc;
		depthStencilDesc.Texture2D.MipSlice = 0;
		depthStencilDesc.Format = m_DepthStencilFormat;
		depthStencilDesc.Flags = D3D12_DSV_FLAG_NONE;
		depthStencilDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

		m_Device->CreateDepthStencilView(m_DsvDescriptor.Get(),
			&depthStencilDesc, m_DsvHeap->GetCPUDescriptorHandleForHeapStart());

		D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_DsvDescriptor.Get(), D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_DEPTH_WRITE);
		m_CommandList->ResourceBarrier(1, &barrier);

		m_CommandList->Close();

		ID3D12CommandList* cmdList[] = { m_CommandList.Get()};
		m_CommandQueue->ExecuteCommandLists(1, cmdList);

		FlushCommandQueue();

		m_ViewPort.TopLeftX = 0.0f;
		m_ViewPort.TopLeftY = 0.0f;
		m_ViewPort.MinDepth = 0.0f;
		m_ViewPort.MaxDepth = 1.0f;
		m_ViewPort.Height = (FLOAT)m_d3dSettings.screenHeight;
		m_ViewPort.Width = (FLOAT)m_d3dSettings.screenWidth;

		m_ScissorRect = { 0, 0, m_d3dSettings.screenWidth, m_d3dSettings.screenHeight };

		m_Camera.SetLens(0.25f * XM_PI, AspectRatio(), 1.0f, 1000.0f);

		DirectX::BoundingFrustum::CreateFromMatrix(m_LocalProjFrustum,
			My_unmove(DirectX::XMLoadFloat4x4(&My_unmove(m_Camera.GetProjectionMatrix()))));
	}

	void D3DApp::EnableDebugLayer() {
		Microsoft::WRL::ComPtr<ID3D12Debug> debugLayer;
		THROWFAILEDIF("@@@ Error: D3D12GetDebugInterface",
			D3D12GetDebugInterface(IID_PPV_ARGS(debugLayer.GetAddressOf())));
		debugLayer->EnableDebugLayer();
	}

	void D3DApp::InitializeConsole() {
		if (AllocConsole() == 0) {
			throw std::runtime_error("Doesn't Alloc Console.");
		}

		FILE* newConIn = nullptr;
		FILE* newConOut = nullptr;
		FILE* newConErr = nullptr;

		freopen_s(&newConIn, "CONIN$", "r", stdin);
		freopen_s(&newConOut, "CONOUT$", "w", stdout);
		freopen_s(&newConErr, "CONOUT$", "w", stderr);

		std::cin.clear();
		std::cout.clear();
		std::cerr.clear();

		std::wcin.clear();
		std::wcout.clear();
		std::wcerr.clear();
	}

	void D3DApp::LogAdapter() {
		IDXGIAdapter* adapter;
		UINT countOf;

		countOf = 0;
		m_Adapters.clear();

		while (m_Factory->EnumAdapters(countOf, &adapter) != DXGI_ERROR_NOT_FOUND) {
			DXGI_ADAPTER_DESC adapterDesc;

			adapter->GetDesc(&adapterDesc);
			WCHAR* description = adapterDesc.Description;
			const SIZE_T videoMemory = (adapterDesc.DedicatedVideoMemory / (1024 * 1024));

			std::wcout << L"*** Adapter: " << description << ", " << videoMemory << " (Mb)" << std::endl;

			m_Adapters.push_back(adapter);
			++countOf;
		}

		std::sort(m_Adapters.begin(), m_Adapters.end(), [](IDXGIAdapter* ial, IDXGIAdapter* iar) {
			return ial > iar;
		});

		std::wcout << std::endl;
	}

	void D3DApp::LogOutput() {
		IDXGIOutput* output;
		UINT countOf;

		countOf = 0;
		m_Outputs.clear();

		for (SIZE_T i = 0; i < m_Adapters.size(); i++) {
			while (m_Adapters[i]->EnumOutputs(countOf, &output) != DXGI_ERROR_NOT_FOUND) {
				DXGI_OUTPUT_DESC outputDesc;

				output->GetDesc(&outputDesc);
				WCHAR* devicename = outputDesc.DeviceName;

				std::wcout << L"*** Output: " << devicename << std::endl;

				m_Outputs.push_back(output);
				++countOf;
			}
		}

		std::sort(m_Outputs.begin(), m_Outputs.end(), [](IDXGIOutput* iol, IDXGIOutput* ior) {
			return iol > ior;
		});

		std::wcout << std::endl;
	}

	void D3DApp::LogModeLists() {
		UINT pNumModes;
		bool Checked;

		Checked = false;
		pNumModes = 0;
		m_ModeLists.clear();

		for (SIZE_T i = 0; i < m_Outputs.size(); i++) {
			m_ModeLists.push_back({});
			m_Outputs[i]->GetDisplayModeList(m_BackBufferFormat, DXGI_ENUM_MODES_INTERLACED, &pNumModes, 0);
			m_ModeLists[i] = std::vector<DXGI_MODE_DESC>(pNumModes);
			m_Outputs[i]->GetDisplayModeList(m_BackBufferFormat, DXGI_ENUM_MODES_INTERLACED, &pNumModes, &m_ModeLists[i][0]);

			for (SIZE_T j = 0; j < m_ModeLists[i].size(); j++) {
				if (m_ModeLists[i][j].Width == (UINT)m_d3dSettings.screenWidth) {
					if (m_ModeLists[i][j].Height == (UINT)m_d3dSettings.screenHeight && !Checked) {
						m_Numerator = m_ModeLists[i][j].RefreshRate.Numerator;
						m_Denominator = m_ModeLists[i][j].RefreshRate.Denominator;

						Checked = true;
					}
				}

				std::wstring wstr;
				wstr.clear();

				wstr += L"*** ModeList " + std::to_wstring(i) + L'\n';
				wstr += L"Width: " + std::to_wstring(m_ModeLists[i][j].Width) + L'\n';
				wstr += L"Height: " + std::to_wstring(m_ModeLists[i][j].Height) + L'\n';
				wstr += L"Resolution: " + std::to_wstring(m_ModeLists[i][j].RefreshRate.Numerator)
					+ L" / " + std::to_wstring(m_ModeLists[i][j].RefreshRate.Denominator) + L'\n';

				std::wcout << wstr << std::endl;
			}

			std::wcout << std::endl;
		}

		std::wcout << std::endl;
	}

	void D3DApp::CreateDevice() {
		HRESULT hadwareResult =
			D3D12CreateDevice(0, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_Device.GetAddressOf()));
		if (FAILED(hadwareResult)) {
			Microsoft::WRL::ComPtr<IDXGIAdapter> warpAdapter;
			THROWFAILEDIF("@@@ Error: IDXGIFactory4::EnumWarpAdapter",
				m_Factory->EnumWarpAdapter(IID_PPV_ARGS(warpAdapter.GetAddressOf())));

			THROWFAILEDIF("@@@ Error: D3D12CreateDevice",
				D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0,
					IID_PPV_ARGS(m_Device.GetAddressOf())));
		}
	}

	void D3DApp::Check4xMsaa() {
		D3D_FEATURE_LEVEL features[] = {
			D3D_FEATURE_LEVEL_1_0_CORE,
			D3D_FEATURE_LEVEL_9_1,
			D3D_FEATURE_LEVEL_9_2,
			D3D_FEATURE_LEVEL_9_3,
			D3D_FEATURE_LEVEL_10_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_12_0,
			D3D_FEATURE_LEVEL_12_1,
			D3D_FEATURE_LEVEL_12_2,
		};

		D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevel;
		featureLevel.NumFeatureLevels = 11;
		featureLevel.pFeatureLevelsRequested = features;
		featureLevel.MaxSupportedFeatureLevel = (D3D_FEATURE_LEVEL)0;

		THROWFAILEDIF("@@@ Error: ID3D12Device::CheckFeatureSupport",
			m_Device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, (void*)&featureLevel, sizeof(featureLevel)));

		m_MinimumFeatureLevel = featureLevel.MaxSupportedFeatureLevel;

		std::string st4xMsaa;
		switch (m_MinimumFeatureLevel) {
		case D3D_FEATURE_LEVEL_1_0_CORE: st4xMsaa = "DirectX 1.0 core"; break;
		case D3D_FEATURE_LEVEL_9_1: st4xMsaa = "DirectX 9.1"; break;
		case D3D_FEATURE_LEVEL_9_2: st4xMsaa = "DirectX 9.2"; break;
		case D3D_FEATURE_LEVEL_9_3: st4xMsaa = "DirectX 9.3"; break;
		case D3D_FEATURE_LEVEL_10_0: st4xMsaa = "DirectX 10.0"; break;
		case D3D_FEATURE_LEVEL_10_1: st4xMsaa = "DirectX 10.1"; break;
		case D3D_FEATURE_LEVEL_11_0: st4xMsaa = "DirectX 11.0"; break;
		case D3D_FEATURE_LEVEL_11_1: st4xMsaa = "DirectX 11.1"; break;
		case D3D_FEATURE_LEVEL_12_0: st4xMsaa = "DirectX 12.0"; break;
		case D3D_FEATURE_LEVEL_12_1: st4xMsaa = "DirectX 12.1"; break;
		case D3D_FEATURE_LEVEL_12_2: st4xMsaa = "DirectX 12.2"; break;
		}

		std::cout << "*** 4xMsaa(MinimumFeatureLevel): " << st4xMsaa << '\n';
		std::cout << std::endl;


		D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevel;
		msQualityLevel.SampleCount = 4;
		msQualityLevel.NumQualityLevels = 0;
		msQualityLevel.Format = m_BackBufferFormat;
		msQualityLevel.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;

		THROWFAILEDIF("@@@ Error: ID3D12Device::CheckFeatureSupport",
			m_Device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, (void*)&msQualityLevel, sizeof(msQualityLevel)));

		m_MultisampleQuality = msQualityLevel.NumQualityLevels;

		std::cout << "*** 4xMsaa(MultisampleQuality): " << m_MultisampleQuality << '\n';
		std::cout << std::endl;
	}

	void D3DApp::CreateFenceAndDescriptorSize() {
		THROWFAILEDIF("@@@ Error: ID3D12Device::CreateFence",
			m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_Fence.GetAddressOf())));

		m_RtvSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		m_DsvSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
		m_CbvSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	void D3DApp::CreateCommandInterface() {
		D3D12_COMMAND_QUEUE_DESC queueDesc;
		memset(&queueDesc, 0x00, sizeof(D3D12_COMMAND_QUEUE_DESC));

		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

		THROWFAILEDIF("@@@ Error: ID3D12Device::CreateCommandAllocator",
			m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(m_CommandAllocator.GetAddressOf())));

		THROWFAILEDIF("@@@ Error: ID3D12Device::CreateCommandList",
			m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_CommandAllocator.Get(),
				0, IID_PPV_ARGS(m_CommandList.GetAddressOf())));

		THROWFAILEDIF("@@@ Error: ID3D12Device::CreateCommandQueue",
			m_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(m_CommandQueue.GetAddressOf())));

		m_CommandList->Close();
	}

	void D3DApp::CreateSwapChain() {
		m_SwapChain.Reset();

		DXGI_SWAP_CHAIN_DESC swapChainDesc;
		swapChainDesc.BufferCount = m_BackBufferCount;

		swapChainDesc.BufferDesc.Width = m_d3dSettings.screenWidth;
		swapChainDesc.BufferDesc.Height = m_d3dSettings.screenHeight;
		swapChainDesc.BufferDesc.Format = m_BackBufferFormat;
		swapChainDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
		swapChainDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
		swapChainDesc.BufferDesc.RefreshRate.Numerator = m_d3dSettings.debugMode ? m_Numerator : 0;
		swapChainDesc.BufferDesc.RefreshRate.Denominator = m_d3dSettings.debugMode ? m_Denominator : 1;

		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
		swapChainDesc.OutputWindow = m_d3dSettings.hwnd;

		swapChainDesc.SampleDesc.Count = m_MSQualityState ? 4 : 1;
		swapChainDesc.SampleDesc.Quality = m_MSQualityState ? (m_MultisampleQuality - 1) : 0;

		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		swapChainDesc.Windowed = m_d3dSettings.fullscreen ? FALSE : TRUE;

		THROWFAILEDIF("@@@ Error: IDXGIFactory4::CreateSwapChain",
			m_Factory->CreateSwapChain(m_CommandQueue.Get(), &swapChainDesc, m_SwapChain.GetAddressOf()));
	}

	void D3DApp::CreateDescriptorHeap() {
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.NodeMask = 0;
		rtvHeapDesc.NumDescriptors = m_BackBufferCount + 6;

		THROWFAILEDIF("@@@ Error: ID3D12Device::CreateDescriptorHeap",
			m_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(m_RtvHeap.GetAddressOf())));

		D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
		dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvHeapDesc.NodeMask = 0;
		dsvHeapDesc.NumDescriptors = 2;

		THROWFAILEDIF("@@@ Error: ID3D12Device::CreateDescriptorHeap",
			m_Device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(m_DsvHeap.GetAddressOf())));

		m_CubeMapDsv = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_DsvHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_DsvSize);
	}

	void D3DApp::CreateCbvDescriptorHeap() {
		D3D12_DESCRIPTOR_HEAP_DESC cbvDesc;
		cbvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		cbvDesc.NodeMask = 0;
		cbvDesc.NumDescriptors = 1;
		cbvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

		THROWFAILEDIF("@@@ Error: ID3D12Device::CreateDescriptorHeap",
			m_Device->CreateDescriptorHeap(&cbvDesc, IID_PPV_ARGS(m_CbvHeap.GetAddressOf())));
	}

	void D3DApp::CreateConstantBufferView() {
		m_UploadObj = std::make_unique<UploadBuffer<UploadObject>>(m_Device.Get(), 1, true);
		UINT cbvByteSize = VertexBuffer::CalcConstantBufferSize(sizeof(UploadObject));

		D3D12_GPU_VIRTUAL_ADDRESS virtualAddress = m_UploadObj->Resource()->GetGPUVirtualAddress();

		UINT cbvElementBegin = 0;
		virtualAddress += cbvElementBegin * cbvByteSize;

		D3D12_CONSTANT_BUFFER_VIEW_DESC ConstantBufferViewDesc;
		ConstantBufferViewDesc.BufferLocation = virtualAddress;
		ConstantBufferViewDesc.SizeInBytes = cbvByteSize;

		m_Device->CreateConstantBufferView(&ConstantBufferViewDesc, m_CbvHeap->GetCPUDescriptorHandleForHeapStart());
	}

	std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> D3DApp::GetStaticSamplers() {

		const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
			0, // shaderRegister
			D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
			D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
			D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
			D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

		const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
			1, // shaderRegister
			D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

		const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
			2, // shaderRegister
			D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
			D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
			D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
			D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

		const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
			3, // shaderRegister
			D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

		const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
			4, // shaderRegister
			D3D12_FILTER_ANISOTROPIC, // filter
			D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
			D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
			D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
			0.0f,                             // mipLODBias
			8);                               // maxAnisotropy

		const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
			5, // shaderRegister
			D3D12_FILTER_ANISOTROPIC, // filter
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
			0.0f,                              // mipLODBias
			8);                                // maxAnisotropy

		return {
			pointWrap, pointClamp,
			linearWrap, linearClamp,
			anisotropicWrap, anisotropicClamp };
	}

	void D3DApp::BuildRootSignature() {
		CD3DX12_DESCRIPTOR_RANGE cubeMapRange;
		cubeMapRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0);

		CD3DX12_DESCRIPTOR_RANGE dRange;
		dRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 9 + 1, 1, 0);

		CD3DX12_ROOT_PARAMETER cbvParameter[5];
		cbvParameter[0].InitAsConstantBufferView(0);		// Object
		cbvParameter[1].InitAsConstantBufferView(1);		// Pass
		cbvParameter[2].InitAsShaderResourceView(0, 1);		// Material
		cbvParameter[3].InitAsDescriptorTable(1, &cubeMapRange, D3D12_SHADER_VISIBILITY_PIXEL); // CubeMap
		cbvParameter[4].InitAsDescriptorTable(1, &dRange, D3D12_SHADER_VISIBILITY_PIXEL); // Textures

		auto sSamplers = GetStaticSamplers();
		
		CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(5, cbvParameter, (UINT)sSamplers.size(), sSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		Microsoft::WRL::ComPtr<ID3DBlob> serialRootSig = nullptr;
		Microsoft::WRL::ComPtr<ID3DBlob> errorMsg = nullptr;
		HRESULT hResult = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialRootSig, &errorMsg);

		if (errorMsg != nullptr) {
			OutputDebugStringA((char*)errorMsg->GetBufferPointer());
		}

		THROWFAILEDIF("@@@ Error: D3D12SerializeRootSignature", hResult);

		hResult = m_Device->CreateRootSignature(0, serialRootSig->GetBufferPointer(),
			serialRootSig->GetBufferSize(), IID_PPV_ARGS(m_RootSignature.GetAddressOf()));

		THROWFAILEDIF("@@@ Error: ID3D12Device::CreateRootSignature", hResult);
	}

	void D3DApp::BuildShadersAndInputlayout() {
		const D3D_SHADER_MACRO opaque[] = {
			"FOG", "1",
			NULL, NULL,
		};

		const D3D_SHADER_MACRO alphatest[] = {
			"FOG", "1",
			"ALPHA_TEST", "1",
			NULL, NULL,
		};

		m_Shaders["standardVS"] = VertexBuffer::CompileShader(SOURCE_SHADER_FILE_VS, nullptr, "VS", "vs_5_1");
		m_Shaders["opaquePS"] = VertexBuffer::CompileShader(SOURCE_SHADER_FILE_PS, &opaque[0], "PS", "ps_5_1");
		m_Shaders["AlphaTestedPS"] = VertexBuffer::CompileShader(SOURCE_SHADER_FILE_PS, &alphatest[0], "PS", "ps_5_1");

		m_Shaders["skyVS"] = VertexBuffer::CompileShader(SOURCE_SHADER_FILE_CUBEMAP_VS, nullptr, "VS", "vs_5_1");
		m_Shaders["skyPS"] = VertexBuffer::CompileShader(SOURCE_SHADER_FILE_CUBEMAP_PS, &opaque[0], "PS", "ps_5_1");

		m_InputElementDesc =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};
	}

	void D3DApp::BuildInstancesTheSkull() {
		std::ifstream fin("skull.txt");

		if (!fin)
		{
			MessageBox(0, L"skull.txt not found.", 0, 0);
			return;
		}

		UINT vcount = 0;
		UINT tcount = 0;
		std::string ignore;

		fin >> ignore >> vcount;
		fin >> ignore >> tcount;
		fin >> ignore >> ignore >> ignore >> ignore;

		XMFLOAT3 vMinf3(+FLT_MAX, +FLT_MAX, +FLT_MAX);
		XMFLOAT3 vMaxf3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

		XMVECTOR vMin = XMLoadFloat3(&vMinf3);
		XMVECTOR vMax = XMLoadFloat3(&vMaxf3);

		std::vector<Vertex> vertices(vcount);
		for (UINT i = 0; i < vcount; ++i)
		{
			fin >> vertices[i].Pos.x >> vertices[i].Pos.y >> vertices[i].Pos.z;
			fin >> vertices[i].Normal.x >> vertices[i].Normal.y >> vertices[i].Normal.z;

			XMVECTOR P = XMLoadFloat3(&vertices[i].Pos);

			XMFLOAT3 spherePos;
			XMStoreFloat3(&spherePos, XMVector3Normalize(P));

			float theta = atan2f(spherePos.z, spherePos.x);

			// [0, 2pi]
			if (theta < 0.0f)
				theta += XM_2PI;

			float phi = acosf(spherePos.y);

			float u = theta / (2.0f * XM_PI);
			float v = phi / XM_PI;

			vertices[i].TexC = { u, v };

			vMin = XMVectorMin(vMin, P);
			vMax = XMVectorMax(vMax, P);
		}

		BoundingBox bounds;
		XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
		XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));

		fin >> ignore;
		fin >> ignore;
		fin >> ignore;

		std::vector<std::int32_t> indices(3 * tcount);
		for (UINT i = 0; i < tcount; ++i)
		{
			fin >> indices[i * 3 + 0] >> indices[i * 3 + 1] >> indices[i * 3 + 2];
		}

		fin.close();

		
		const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);

		const UINT ibByteSize = (UINT)indices.size() * sizeof(std::int32_t);

		auto geo = std::make_unique<MeshGeometry>();
		geo->Name = "lskullGeo";

		THROWFAILEDIF("@@@ Error: D3DCreateBlob", D3DCreateBlob(vbByteSize, &geo->CPUVertexBuffer));
		CopyMemory(geo->CPUVertexBuffer->GetBufferPointer(), vertices.data(), vbByteSize);

		THROWFAILEDIF("@@@ Error: D3DCreateBlob", D3DCreateBlob(ibByteSize, &geo->CPUIndexBuffer));
		CopyMemory(geo->CPUIndexBuffer->GetBufferPointer(), indices.data(), ibByteSize);

		geo->GPUVertexBuffer = VertexBuffer::CreateDefaultBuffer(m_Device.Get(),
			m_CommandList.Get(), vertices.data(), vbByteSize, geo->GPUVertexUploader);

		geo->GPUIndexBuffer = VertexBuffer::CreateDefaultBuffer(m_Device.Get(),
			m_CommandList.Get(), indices.data(), ibByteSize, geo->GPUIndexUploader);

		geo->VertexByteStride = sizeof(Vertex);
		geo->VertexBufferByteSize = vbByteSize;
		geo->IndexFormat = DXGI_FORMAT_R32_UINT;
		geo->IndexBufferByteSize = ibByteSize;

		SubMeshGeometry submesh;
		submesh.IndexCount = (UINT)indices.size();
		submesh.StartIndexLocation = 0;
		submesh.BaseVertexLocation = 0;
		submesh.Bounds = bounds;

		geo->DrawArgs["lskull"] = submesh;

		m_DrawArgs[geo->Name] = std::move(geo);
	}

	void D3DApp::BuildPSO() {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC GrphicsPSODesc = {};

		GrphicsPSODesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		GrphicsPSODesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		GrphicsPSODesc.DSVFormat = m_DepthStencilFormat;
		GrphicsPSODesc.InputLayout = { m_InputElementDesc.data(), (UINT)m_InputElementDesc.size() };
		GrphicsPSODesc.NumRenderTargets = 1;
		GrphicsPSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		GrphicsPSODesc.pRootSignature = m_RootSignature.Get();
		GrphicsPSODesc.VS = {
			reinterpret_cast<BYTE*>(m_Shaders["standardVS"]->GetBufferPointer()),
			m_Shaders["standardVS"]->GetBufferSize(),
		};
		GrphicsPSODesc.PS = {
			reinterpret_cast<BYTE*>(m_Shaders["opaquePS"]->GetBufferPointer()),
			m_Shaders["opaquePS"]->GetBufferSize(),
		};
		GrphicsPSODesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		GrphicsPSODesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		GrphicsPSODesc.RTVFormats[0] = m_BackBufferFormat;
		GrphicsPSODesc.SampleDesc.Count = m_MSQualityState ? 4 : 1;
		GrphicsPSODesc.SampleDesc.Quality = m_MSQualityState ? (m_MultisampleQuality - 1) : 0;
		GrphicsPSODesc.SampleMask = UINT_MAX;
		
		THROWFAILEDIF("@@@Error: ID3D12Device::CreateGraphicsPipelineState",
			m_Device->CreateGraphicsPipelineState(&GrphicsPSODesc, IID_PPV_ARGS(m_PSOs["opaque"].GetAddressOf())));


		D3D12_GRAPHICS_PIPELINE_STATE_DESC WireframePSODesc = GrphicsPSODesc;
		WireframePSODesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
		THROWFAILEDIF("@@@Error: ID3D12Device::CreateGraphicsPipelineState",
			m_Device->CreateGraphicsPipelineState(&WireframePSODesc, IID_PPV_ARGS(m_PSOs["opaque_wireframe"].GetAddressOf())));

		D3D12_GRAPHICS_PIPELINE_STATE_DESC HighlightPSODesc = GrphicsPSODesc;

		D3D12_RENDER_TARGET_BLEND_DESC blendDesc = {};
		blendDesc.LogicOpEnable = FALSE;				// logicop
		blendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
		blendDesc.BlendEnable = TRUE;					// blendop
		blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
		blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		blendDesc.SrcBlend = D3D12_BLEND_ONE;
		blendDesc.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
		blendDesc.DestBlend = D3D12_BLEND_ZERO;
		blendDesc.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		HighlightPSODesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		HighlightPSODesc.BlendState.RenderTarget[0] = blendDesc;

		THROWFAILEDIF("@@@Error: ID3D12Device::CreateGraphicsPipelineState",
			m_Device->CreateGraphicsPipelineState(&HighlightPSODesc, IID_PPV_ARGS(m_PSOs["highlight"].GetAddressOf())));


		D3D12_GRAPHICS_PIPELINE_STATE_DESC SkyPSODesc = GrphicsPSODesc;
		SkyPSODesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		SkyPSODesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		SkyPSODesc.VS = {
			reinterpret_cast<BYTE*>(m_Shaders["skyVS"]->GetBufferPointer()),
			m_Shaders["skyVS"]->GetBufferSize(),
		};
		SkyPSODesc.PS = {
			reinterpret_cast<BYTE*>(m_Shaders["skyPS"]->GetBufferPointer()),
			m_Shaders["skyPS"]->GetBufferSize(),
		};

		THROWFAILEDIF("@@@Error: ID3D12Device::CreateGraphicsPipelineState",
			m_Device->CreateGraphicsPipelineState(&SkyPSODesc, IID_PPV_ARGS(m_PSOs["sky"].GetAddressOf())));
	}


	void D3DApp::BuildFrameResources() {
		for (int i = 0; i < gNumFrameResources; ++i) {
			m_FrameResources.push_back(std::make_unique<FrameResource>(m_Device.Get(),
				(1 + 6), (UINT)m_AllRItems.size(), (UINT)m_Materials.size()));
		}
	}

	void D3DApp::UpdateObjectCB(const GameTimer* gameTimer) {
		auto currInstanceBuffer = m_CurrFrameResource->m_InstCB.get();

		XMMATRIX invView = XMMatrixInverse(
			&My_unmove(XMMatrixDeterminant(My_unmove(XMLoadFloat4x4(&My_unmove(m_Camera.GetViewMatrix()))))),
			My_unmove(XMLoadFloat4x4(&My_unmove(m_Camera.GetViewMatrix()))));

		size_t ObjectCounts = 0;

		for (auto& e : m_AllRItems) {
			XMMATRIX world = XMLoadFloat4x4(&e->World);
			XMMATRIX tex = XMLoadFloat4x4(&e->TexTransform);

			XMMATRIX invWorld = XMMatrixInverse(&My_unmove(XMMatrixDeterminant(world)), world);

			XMMATRIX toLocal = XMMatrixMultiply(invView, invWorld);

			BoundingFrustum bFrustum;
			m_LocalProjFrustum.Transform(bFrustum, toLocal);

			if ((bFrustum.Contains(e->Bounds) != DirectX::DISJOINT) || m_isFrustumCulling) {
				InstanceConstants instanceConstants;
				XMStoreFloat4x4(&instanceConstants.World, XMMatrixTranspose(world));
				XMStoreFloat4x4(&instanceConstants.TexTransform, XMMatrixTranspose(tex));
				instanceConstants.MaterialIndex = e->Mat->MatCBIndex;

				currInstanceBuffer->CopyData(e->InstanceCount, instanceConstants);
				ObjectCounts++;
			}
		}

		m_SkullCounts = (UINT)ObjectCounts;
	}

	void D3DApp::UpdateMatetialCBs(const GameTimer* gameTimer) {
		auto currMaterialCB = m_CurrFrameResource->m_MatVB.get();

		for (auto& e : m_Materials) {
			Material* m = e.second.get();

			if (m->NumFramesDirty > 0) {
				XMMATRIX matTransform = XMLoadFloat4x4(&m->MatTransform);

				MaterialConstants mConstants;
				XMStoreFloat4x4(&mConstants.MatTransform, XMMatrixTranspose(matTransform));
				mConstants.DiffuseAlbedo = m->DiffuseAlbedo;
				mConstants.FresnelR0 = m->FresnelR0;
				mConstants.Roughness = m->Roughness;
				mConstants.DiffuseMapIndex = m->DiffuseSrvHeapIndex;
				mConstants.NormalSrvHeapIndex = m->NormalSrvHeapIndex;

				currMaterialCB->CopyData(m->MatCBIndex, mConstants);

				--(m->NumFramesDirty);
			}
		}
	}

	void D3DApp::OnKeyboardInput(const GameTimer* gameTimer) {
		const float dt = gameTimer->DeltaTime();
		const float walkSpeed = 30.0f;

		if (GetAsyncKeyState('W') & 0x8000)
			m_Camera.Walk(dt * walkSpeed);
		if (GetAsyncKeyState('S') & 0x8000)
			m_Camera.Walk(-dt * walkSpeed);
		if (GetAsyncKeyState('A') & 0x8000)
			m_Camera.Strafe(-dt * walkSpeed);
		if (GetAsyncKeyState('D') & 0x8000)
			m_Camera.Strafe(dt * walkSpeed);

		m_Camera.UpdateViewMatrix();

		if (GetAsyncKeyState('Z') & 0x8000)
			m_IsWireFrames = true;
		else if (GetAsyncKeyState('X') & 0x8000)
			m_IsWireFrames = false;

		if (GetAsyncKeyState('N') & 0x8000)
			m_SnowCubeMapTextureIndex = 4;
		else if (GetAsyncKeyState('M') & 0x8000)
			m_SnowCubeMapTextureIndex = 0;

		if (GetAsyncKeyState('F') & 0x8000)
			m_isFrustumCulling = !m_isFrustumCulling;

		if (GetAsyncKeyState('P') & 0x8000)
			m_PickingFromAll = !m_PickingFromAll;
	}

	void D3DApp::UpdateWindowTitle(const GameTimer* gameTimer) {
		static float elapsedTime = 0.0f;
		static int fps = 0;

		++fps;
		if (gameTimer->TotalTime() - elapsedTime >= 1.0f) {
			char buf[0x50] = {};
			sprintf_s(buf, "Object Count: %d, Fps: %d, FrustumCulling: %s, PickingFromAll: %s",
				m_SkullCounts,
				fps,
				(m_isFrustumCulling ? "Off" : "On"),
				(m_PickingFromAll ? "On" : "Off"));

			SetWindowTextA(m_d3dSettings.hwnd, buf);

			elapsedTime += 1.0f;
			fps = 0;
		}
	}

	void D3DApp::UpdateSkullPosition(float dt) {
		const static XMVECTOR m_SkullCenter = { 10.0f, 0.0f, 0.0f };
		const float orbit_speed = 2.0f;
		const float radius = 15.0f;
		float angle = 0.0f;

		angle += orbit_speed * dt;

		XMFLOAT4 xmfloat4;
		XMStoreFloat4(&xmfloat4, m_SkullCenter);
		XMStoreFloat4x4(&m_SkullRitem->World, XMMatrixTranslation(xmfloat4.x + radius * cosf(angle), 0.0f, xmfloat4.z + radius * sinf(angle)));
		m_SkullRitem->NumFramesDirty = gNumFrameResources;
	}

	void D3DApp::UpdatePassCB() {
		XMMATRIX view = XMLoadFloat4x4(&My_unmove(m_Camera.GetViewMatrix()));
		XMMATRIX proj = XMLoadFloat4x4(&My_unmove(m_Camera.GetProjectionMatrix()));

		XMMATRIX viewproj = XMMatrixMultiply(view, proj);

		XMFLOAT2 RTSize = XMFLOAT2((float)m_d3dSettings.screenWidth, (float)m_d3dSettings.screenHeight);
		XMFLOAT2 InvRTSize = XMFLOAT2(1.0f / (float)m_d3dSettings.screenWidth, 1.0f / (float)m_d3dSettings.screenHeight);

		XMMATRIX InvViewproj = XMMatrixInverse(&My_unmove(XMMatrixDeterminant(viewproj)), viewproj);
		XMMATRIX InvView = XMMatrixInverse(&My_unmove(XMMatrixDeterminant(view)), view);
		XMMATRIX InvProj = XMMatrixInverse(&My_unmove(XMMatrixDeterminant(proj)), proj);


		m_MainPassCB.RTSize = RTSize;
		m_MainPassCB.InvRTSize = InvRTSize;
		m_MainPassCB.EyePosW = m_Camera.GetPosition();

		XMStoreFloat4x4(&m_MainPassCB.View, XMMatrixTranspose(view));
		XMStoreFloat4x4(&m_MainPassCB.ViewProj, XMMatrixTranspose(viewproj));
		XMStoreFloat4x4(&m_MainPassCB.Proj, XMMatrixTranspose(proj));
		XMStoreFloat4x4(&m_MainPassCB.InvViewProj, XMMatrixTranspose(InvViewproj));
		XMStoreFloat4x4(&m_MainPassCB.InvView, XMMatrixTranspose(InvView));
		XMStoreFloat4x4(&m_MainPassCB.InvProj, XMMatrixTranspose(InvProj));

		m_MainPassCB.NearZ = 1.0f;
		m_MainPassCB.FarZ = 1000.0f;

		m_MainPassCB.DeltaTime = (float)((*m_d3dSettings.gameTimer)->DeltaTime());
		m_MainPassCB.TotalTime = (float)((*m_d3dSettings.gameTimer)->TotalTime());

		m_MainPassCB.AmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };
		m_MainPassCB.Lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
		m_MainPassCB.Lights[0].Strength = { 0.6f, 0.6f, 0.6f };
		m_MainPassCB.Lights[1].Direction = { -0.57735f, -0.57735f, 0.57735f };
		m_MainPassCB.Lights[1].Strength = { 0.3f, 0.3f, 0.3f };
		m_MainPassCB.Lights[2].Direction = { 0.0f, -0.707f, -0.707f };
		m_MainPassCB.Lights[2].Strength = { 0.15f, 0.15f, 0.15f };


		auto currPassCB = m_CurrFrameResource->m_PassCB.get();
		currPassCB->CopyData(0, m_MainPassCB);

		UpdateDynamicFaceCameraPassCBs();
	}

	void D3DApp::UpdateDynamicFaceCameraPassCBs() {
		for (int i = 0; i < 6; ++i) {
			PassConstants faceCameraPass = m_MainPassCB;


			XMMATRIX V = XMLoadFloat4x4(&My_unmove(m_DynamicCubemapCamera[i].GetViewMatrix()));
			XMMATRIX P = XMLoadFloat4x4(&My_unmove(m_DynamicCubemapCamera[i].GetProjectionMatrix()));

			XMMATRIX invV = XMMatrixInverse(&My_unmove(XMMatrixDeterminant(V)), V);
			XMMATRIX invP = XMMatrixInverse(&My_unmove(XMMatrixDeterminant(P)), P);

			XMMATRIX VP = XMMatrixMultiply(V, P);
			XMMATRIX invVP = XMMatrixInverse(&My_unmove(XMMatrixDeterminant(VP)), VP);

			XMStoreFloat4x4(&faceCameraPass.View, XMMatrixTranspose(V));
			XMStoreFloat4x4(&faceCameraPass.Proj, XMMatrixTranspose(P));
			XMStoreFloat4x4(&faceCameraPass.InvView, XMMatrixTranspose(invV));
			XMStoreFloat4x4(&faceCameraPass.ViewProj, XMMatrixTranspose(VP));
			XMStoreFloat4x4(&faceCameraPass.InvViewProj, XMMatrixTranspose(invVP));
			faceCameraPass.EyePosW = m_DynamicCubemapCamera[i].GetPosition();

			faceCameraPass.RTSize = { (float)gCubemapSize, (float)gCubemapSize };
			faceCameraPass.InvRTSize = { (1.0f / gCubemapSize), (1.0f / gCubemapSize) };

			auto pass = m_CurrFrameResource->m_PassCB.get();
			pass->CopyData(1 + i, faceCameraPass);
		}
	}

	void D3DApp::BuildCubeFaceCamera(float x, float y, float z) {
		XMFLOAT3 center(x, y, z);
		XMFLOAT3 worldUp(0.0f, 1.0f, 0.0f);

		XMFLOAT3 targets[6] = {
			{ x + 1.0f, y + 0.0f, z + 0.0f },	// X+
			{ x - 1.0f, y + 0.0f, z + 0.0f },	// X-
			{ x + 0.0f, y + 1.0f, z + 0.0f },	// Y+
			{ x + 0.0f, y - 1.0f, z + 0.0f },	// Y-
			{ x + 0.0f, y + 0.0f, z + 1.0f },	// Z+
			{ x + 0.0f, y + 0.0f, z - 1.0f },	// Z-
		};

		XMFLOAT3 ups[6] = {
			{ 0.0f, 1.0f, 0.0f },	// X+
			{ 0.0f, 1.0f, 0.0f },	// X-
			{ 0.0f, 0.0f, -1.0f },	// Y+
			{ 0.0f, 0.0f, 1.0f },	// Y-
			{ 0.0f, 1.0f, 0.0f },	// Z+
			{ 0.0f, 1.0f, 0.0f },	// Z-
		};

		for (int i = 0; i < 6; ++i) {
			m_DynamicCubemapCamera[i].Lookat(center, targets[i], ups[i]);
			m_DynamicCubemapCamera[i].SetLens(0.5f * XM_PI, 1.0f, 0.1f, 1000.0f);
			m_DynamicCubemapCamera[i].UpdateViewMatrix();
		}
	}

	void D3DApp::LoadTexture() {
		auto bricksTex = std::make_unique<Texture>();
		bricksTex->name = "bricksTex";
		bricksTex->filename = L"./bricks.dds";
		THROWFAILEDIF("@@@ Error: CreateDDSTextureFromFile12", DirectX::CreateDDSTextureFromFile12(m_Device.Get(),
			m_CommandList.Get(), bricksTex->filename.c_str(),
			bricksTex->GPUResource, bricksTex->GPUUploader));

		auto checkboardTex = std::make_unique<Texture>();
		checkboardTex->name = "checkboardTex";
		checkboardTex->filename = L"./checkboard.dds";
		THROWFAILEDIF("@@@ Error: CreateDDSTextureFromFile12", DirectX::CreateDDSTextureFromFile12(m_Device.Get(),
			m_CommandList.Get(), checkboardTex->filename.c_str(),
			checkboardTex->GPUResource, checkboardTex->GPUUploader));

		auto grassTex = std::make_unique<Texture>();
		grassTex->name = "grassTex";
		grassTex->filename = L"./grasscube1024.dds";
		THROWFAILEDIF("@@@ Error: DirectX::CreateDDSTextureFromFile12", DirectX::CreateDDSTextureFromFile12(m_Device.Get(), m_CommandList.Get(),
			grassTex->filename.c_str(), grassTex->GPUResource, grassTex->GPUUploader));

		auto waterTex = std::make_unique<Texture>();
		waterTex->name = "waterTex";
		waterTex->filename = L"./water.dds";
		THROWFAILEDIF("@@@ Error: DirectX::CreateDDSTextureFromFile12", DirectX::CreateDDSTextureFromFile12(m_Device.Get(), m_CommandList.Get(),
			waterTex->filename.c_str(), waterTex->GPUResource, waterTex->GPUUploader));

		auto fenceTex = std::make_unique<Texture>();
		fenceTex->name = "fenceTex";
		fenceTex->filename = L"./wirefence.dds";
		THROWFAILEDIF("@@@ Error: DirectX::CreateDDSTextureFromFile12", DirectX::CreateDDSTextureFromFile12(m_Device.Get(), m_CommandList.Get(),
			fenceTex->filename.c_str(), fenceTex->GPUResource, fenceTex->GPUUploader));


		auto white1x1Tex = std::make_unique<Texture>();
		white1x1Tex->name = "white1x1";
		white1x1Tex->filename = L"./white1x1.dds";
		THROWFAILEDIF("@@@ Error: DirectX::CreateDDSTextureFromFile12", DirectX::CreateDDSTextureFromFile12(m_Device.Get(), m_CommandList.Get(),
			white1x1Tex->filename.c_str(), white1x1Tex->GPUResource, white1x1Tex->GPUUploader));

		auto iceTex = std::make_unique<Texture>();
		iceTex->name = "ice";
		iceTex->filename = L"./snowcube1024.dds";
		THROWFAILEDIF("@@@ Error: DirectX::CreateDDSTextureFromFile12", DirectX::CreateDDSTextureFromFile12(m_Device.Get(), m_CommandList.Get(),
			iceTex->filename.c_str(), iceTex->GPUResource, iceTex->GPUUploader));

		auto bricks2Tex = std::make_unique<Texture>();
		bricks2Tex->name = "bricks2";
		bricks2Tex->filename = L"./bricks2.dds";
		THROWFAILEDIF("@@@ Error: DirectX::CreateDDSTextureFromFile12", DirectX::CreateDDSTextureFromFile12(m_Device.Get(), m_CommandList.Get(),
			bricks2Tex->filename.c_str(), bricks2Tex->GPUResource, bricks2Tex->GPUUploader));

		auto bricks2NormTex = std::make_unique<Texture>();
		bricks2NormTex->name = "bricks2NormTex";
		bricks2NormTex->filename = L"./bricks2_nmap.dds";
		THROWFAILEDIF("@@@ Error: DirectX::CreateDDSTextureFromFile12", DirectX::CreateDDSTextureFromFile12(m_Device.Get(), m_CommandList.Get(),
			bricks2NormTex->filename.c_str(), bricks2NormTex->GPUResource, bricks2NormTex->GPUUploader));

		m_Textures[grassTex->name] = std::move(grassTex);
		m_Textures[waterTex->name] = std::move(waterTex);
		m_Textures[fenceTex->name] = std::move(fenceTex);

		m_Textures[white1x1Tex->name] = std::move(white1x1Tex);
		m_Textures[iceTex->name] = std::move(iceTex);
		m_Textures[bricksTex->name] = std::move(bricksTex);
		m_Textures[checkboardTex->name] = std::move(checkboardTex);

		m_Textures[bricks2Tex->name] = std::move(bricks2Tex);
		m_Textures[bricks2NormTex->name] = std::move(bricks2NormTex);
	}

	void D3DApp::BuildRenderItems() {
		auto lSkullRitems = std::make_unique<RenderItem>();
		lSkullRitems->World = VertexBuffer::GetMatrixIdentity4x4();
		lSkullRitems->TexTransform = VertexBuffer::GetMatrixIdentity4x4();
		lSkullRitems->InstanceCount = 0;
		lSkullRitems->Mat = m_Materials["white1x1"].get();
		lSkullRitems->Geo = m_DrawArgs["lskullGeo"].get();
		lSkullRitems->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		lSkullRitems->IndexCount = lSkullRitems->Geo->DrawArgs["lskull"].IndexCount;
		lSkullRitems->StartIndexLocation = lSkullRitems->Geo->DrawArgs["lskull"].StartIndexLocation;
		lSkullRitems->BaseVertexLocation = lSkullRitems->Geo->DrawArgs["lskull"].BaseVertexLocation;
		lSkullRitems->Bounds = lSkullRitems->Geo->DrawArgs["lskull"].Bounds;
		lSkullRitems->Visible = true;

		m_SkullRitem = lSkullRitems.get();
		mRitemLayer[(int)RenderLayer::Skull].push_back(lSkullRitems.get());
		m_AllRItems.push_back(std::move(lSkullRitems));


		auto HighlightRitem = std::make_unique<RenderItem>();
		HighlightRitem->World = VertexBuffer::GetMatrixIdentity4x4();
		HighlightRitem->TexTransform = VertexBuffer::GetMatrixIdentity4x4();
		HighlightRitem->InstanceCount = 1;
		HighlightRitem->Mat = m_Materials["yell"].get();
		HighlightRitem->Geo = m_DrawArgs["lskullGeo"].get();
		HighlightRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		HighlightRitem->Visible = false;

		HighlightRitem->IndexCount = 0;
		HighlightRitem->StartIndexLocation = 0;
		HighlightRitem->BaseVertexLocation = 0;

		m_PickedItem = HighlightRitem.get();
		mRitemLayer[(int)RenderLayer::Highlight].push_back(HighlightRitem.get());
		m_AllRItems.push_back(std::move(HighlightRitem));

		auto SkySphereRitem = std::make_unique<RenderItem>();
		XMStoreFloat4x4(&SkySphereRitem->World, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
		SkySphereRitem->TexTransform = VertexBuffer::GetMatrixIdentity4x4();
		SkySphereRitem->InstanceCount = 2;
		SkySphereRitem->Mat = m_Materials["ice"].get();
		SkySphereRitem->Geo = m_DrawArgs["shapeGeo"].get();
		SkySphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		SkySphereRitem->IndexCount = SkySphereRitem->Geo->DrawArgs["sphere"].IndexCount;
		SkySphereRitem->StartIndexLocation = SkySphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
		SkySphereRitem->BaseVertexLocation = SkySphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
		SkySphereRitem->Visible = true;

		mRitemLayer[(int)RenderLayer::Sky].push_back(SkySphereRitem.get());
		m_AllRItems.push_back(std::move(SkySphereRitem));


		UINT Count = 3;
		for (int i = 0; i < 9; ++i) {
			auto SphereRitem = std::make_unique<RenderItem>();
			XMStoreFloat4x4(&SphereRitem->World, XMMatrixScaling(10.0f, 10.0f, 10.0f) * XMMatrixTranslation(10.0f + (i * 13), 0.0f, 0.0f));
			SphereRitem->TexTransform = VertexBuffer::GetMatrixIdentity4x4();
			SphereRitem->InstanceCount = Count++;
			std::string MaterialString = "mirror" + std::to_string(i);
			SphereRitem->Mat = m_Materials[MaterialString].get();
			SphereRitem->Geo = m_DrawArgs["shapeGeo"].get();
			SphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			SphereRitem->IndexCount = SphereRitem->Geo->DrawArgs["sphere"].IndexCount;
			SphereRitem->StartIndexLocation = SphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
			SphereRitem->BaseVertexLocation = SphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
			SphereRitem->Bounds = SphereRitem->Geo->DrawArgs["sphere"].Bounds;
			SphereRitem->Visible = true;

			if (i == 0) {
				mRitemLayer[(int)RenderLayer::DynamicCubemapOpaque].push_back(SphereRitem.get());
				XMStoreFloat4x4(&SphereRitem->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
			}
			else {
				mRitemLayer[(int)RenderLayer::Opaque].push_back(SphereRitem.get());
			}
			m_AllRItems.push_back(std::move(SphereRitem));
		}

		// 12 ->
		auto PlaneGridRitem = std::make_unique<RenderItem>();
		XMStoreFloat4x4(&PlaneGridRitem->World, XMMatrixScaling(1.0f, 1.0f, 1.0f) * XMMatrixTranslation(0.0f, -15.0f, 0.0f));
		PlaneGridRitem->TexTransform = VertexBuffer::GetMatrixIdentity4x4();
		PlaneGridRitem->InstanceCount = 12;
		PlaneGridRitem->Mat = m_Materials["planeWall1"].get();
		PlaneGridRitem->Geo = m_DrawArgs["planeGeo"].get();
		PlaneGridRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		PlaneGridRitem->IndexCount = PlaneGridRitem->Geo->DrawArgs["grid"].IndexCount;
		PlaneGridRitem->StartIndexLocation = PlaneGridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
		PlaneGridRitem->BaseVertexLocation = PlaneGridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
		PlaneGridRitem->Visible = true;

		auto PlaneGridRitem2 = std::make_unique<RenderItem>();
		XMStoreFloat4x4(&PlaneGridRitem2->World, XMMatrixScaling(1.0f, 1.0f, 1.0f) * XMMatrixTranslation(-30.0f, -15.0f, 0.0f));
		PlaneGridRitem2->TexTransform = VertexBuffer::GetMatrixIdentity4x4();
		PlaneGridRitem2->InstanceCount = 13;
		PlaneGridRitem2->Mat = m_Materials["planeWall2"].get();
		PlaneGridRitem2->Geo = m_DrawArgs["planeGeo"].get();
		PlaneGridRitem2->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		PlaneGridRitem2->IndexCount = PlaneGridRitem2->Geo->DrawArgs["grid"].IndexCount;
		PlaneGridRitem2->StartIndexLocation = PlaneGridRitem2->Geo->DrawArgs["grid"].StartIndexLocation;
		PlaneGridRitem2->BaseVertexLocation = PlaneGridRitem2->Geo->DrawArgs["grid"].BaseVertexLocation;
		PlaneGridRitem2->Visible = true;

		mRitemLayer[(int)RenderLayer::Opaque].push_back(PlaneGridRitem.get());
		m_AllRItems.push_back(std::move(PlaneGridRitem));
		mRitemLayer[(int)RenderLayer::Opaque].push_back(PlaneGridRitem2.get());
		m_AllRItems.push_back(std::move(PlaneGridRitem2));
	}

	void D3DApp::BuildDescriptorHeaps() {
		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
		srvHeapDesc.NodeMask = 0;
		srvHeapDesc.NumDescriptors = (9 + 1);
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

		THROWFAILEDIF("@@@ Error: CreateDescriptorHeap(D3DApp::BuildDescriptorHeaps)",
			m_Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_SrvDescriptorHeap)));

		CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

		auto grassTex = m_Textures["grassTex"]->GPUResource;

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		srvDesc.Texture2D.MipLevels = grassTex->GetDesc().MipLevels;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;

		srvDesc.Format = grassTex->GetDesc().Format;
		m_Device->CreateShaderResourceView(grassTex.Get(), &srvDesc, srvHandle);
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;

		srvHandle.Offset(1, m_CbvSize);

		auto waterTex = m_Textures["waterTex"]->GPUResource;
		srvDesc.Format = waterTex->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = waterTex->GetDesc().MipLevels;
		m_Device->CreateShaderResourceView(waterTex.Get(), &srvDesc, srvHandle);

		srvHandle.Offset(1, m_CbvSize);

		auto fenceTex = m_Textures["fenceTex"]->GPUResource;
		srvDesc.Format = fenceTex->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = fenceTex->GetDesc().MipLevels;
		m_Device->CreateShaderResourceView(fenceTex.Get(), &srvDesc, srvHandle);


		srvHandle.Offset(1, m_CbvSize);

		auto white1x1Tex = m_Textures["white1x1"]->GPUResource;
		srvDesc.Format = white1x1Tex->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = white1x1Tex->GetDesc().MipLevels;
		m_Device->CreateShaderResourceView(white1x1Tex.Get(), &srvDesc, srvHandle);

		srvHandle.Offset(1, m_CbvSize);

		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		auto iceTex = m_Textures["ice"]->GPUResource;
		srvDesc.Format = iceTex->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = iceTex->GetDesc().MipLevels;
		m_Device->CreateShaderResourceView(iceTex.Get(), &srvDesc, srvHandle);
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;

		srvHandle.Offset(1, m_CbvSize);

		m_SnowCubeMapTextureIndex = 4;
		
		auto checkboardTex = m_Textures["checkboardTex"]->GPUResource;
		srvDesc.Format = checkboardTex->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = checkboardTex->GetDesc().MipLevels;
		m_Device->CreateShaderResourceView(checkboardTex.Get(), &srvDesc, srvHandle);
		
		srvHandle.Offset(1, m_CbvSize);

		auto bricksTex = m_Textures["bricksTex"]->GPUResource;
		srvDesc.Format = bricksTex->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = bricksTex->GetDesc().MipLevels;
		m_Device->CreateShaderResourceView(bricksTex.Get(), &srvDesc, srvHandle);

		srvHandle.Offset(1, m_CbvSize);

		auto SrvCpuhandle = m_SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
		auto SrvGpuhandle = m_SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
		auto RtvCpuhandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
		const int rtvOffset = m_BackBufferCount;
		m_DynamicCubemapIndex = 7;

		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle[6];

		for (int i = 0; i < 6; ++i) {
			rtvHandle[i] = CD3DX12_CPU_DESCRIPTOR_HANDLE(RtvCpuhandle, i + rtvOffset, m_RtvSize);
		}

		m_CubeRenderTarget->BuildDescriptor(
			CD3DX12_CPU_DESCRIPTOR_HANDLE(SrvCpuhandle, m_DynamicCubemapIndex, m_CbvSize),
			CD3DX12_GPU_DESCRIPTOR_HANDLE(SrvGpuhandle, m_DynamicCubemapIndex, m_CbvSize),
			rtvHandle);

		srvHandle.Offset(1, m_CbvSize);

		auto bricks2Tex = m_Textures["bricks2"]->GPUResource;
		srvDesc.Format = bricks2Tex->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = bricks2Tex->GetDesc().MipLevels;
		m_Device->CreateShaderResourceView(bricks2Tex.Get(), &srvDesc, srvHandle);

		srvHandle.Offset(1, m_CbvSize);

		auto bricks2NormTex = m_Textures["bricks2NormTex"]->GPUResource;
		srvDesc.Format = bricks2NormTex->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = bricks2NormTex->GetDesc().MipLevels;
		m_Device->CreateShaderResourceView(bricks2NormTex.Get(), &srvDesc, srvHandle);
	}

	void D3DApp::BuildDynamicDepthStencil() {
		D3D12_RESOURCE_DESC dsvDesc = {};
		dsvDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		dsvDesc.Alignment = 0;
		dsvDesc.DepthOrArraySize = 1;
		dsvDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		dsvDesc.Format = m_DepthStencilFormat;
		dsvDesc.Height = gCubemapSize;
		dsvDesc.Width = gCubemapSize;
		dsvDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		dsvDesc.MipLevels = 1;
		dsvDesc.SampleDesc.Count = 1;
		dsvDesc.SampleDesc.Quality = 0;

		D3D12_CLEAR_VALUE clearValue = {};
		clearValue.DepthStencil.Depth = 1.0f;
		clearValue.DepthStencil.Stencil = 0;
		clearValue.Format = m_DepthStencilFormat;

		THROWFAILEDIF("@@@ Error: BuildDynamicDepthStencil=>CreateCommittedResource", m_Device->CreateCommittedResource(
			&My_unmove(CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT)),
			D3D12_HEAP_FLAG_NONE,
			&dsvDesc,
			D3D12_RESOURCE_STATE_COMMON,
			&clearValue,
			IID_PPV_ARGS(m_DynamicDepthStencilBuffer.GetAddressOf())
		));

		m_Device->CreateDepthStencilView(m_DynamicDepthStencilBuffer.Get(), nullptr, m_CubeMapDsv);
		m_CommandList->ResourceBarrier(1, &My_unmove(CD3DX12_RESOURCE_BARRIER::Transition(
			m_DynamicDepthStencilBuffer.Get(),
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_DEPTH_WRITE
		)));
	}

	void D3DApp::BuildMaterials() {
		auto grass = std::make_unique<Material>();
		grass->Name = "grass";
		grass->DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
		grass->FresnelR0 = { 0.01f, 0.01f, 0.01f };
		grass->Roughness = 0.125f;
		grass->MatCBIndex = 0;
		grass->DiffuseSrvHeapIndex = 0;

		auto water = std::make_unique<Material>(); //
		water->Name = "water";
		water->DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 0.5f };
		water->FresnelR0 = { 0.2f, 0.2f, 0.2f };
		water->Roughness = 0.0f;
		water->MatCBIndex = 1;
		water->DiffuseSrvHeapIndex = 1;

		auto wirefence = std::make_unique<Material>(); //
		wirefence->Name = "wirefence";
		wirefence->MatCBIndex = 2;
		wirefence->DiffuseSrvHeapIndex = 2;
		wirefence->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
		wirefence->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
		wirefence->Roughness = 0.25f;


		auto white1x1 = std::make_unique<Material>();
		white1x1->Name = "white1x1";
		white1x1->DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
		white1x1->FresnelR0 = { 0.05f, 0.05f, 0.05f };
		white1x1->Roughness = 0.3f;
		white1x1->MatCBIndex = 3;
		white1x1->DiffuseSrvHeapIndex = 3;

		auto mirror0 = std::make_unique<Material>();
		mirror0->Name = "mirror0";
		mirror0->MatCBIndex = 4;
		mirror0->DiffuseSrvHeapIndex = 3;
		mirror0->DiffuseAlbedo = XMFLOAT4(0.0f, 0.0f, 1.0f, 1.0f);
		mirror0->FresnelR0 = XMFLOAT3(0.98f, 0.97f, 0.95f);
		mirror0->Roughness = 0.1f;

		auto ice = std::make_unique<Material>();
		ice->Name = "ice";
		ice->MatCBIndex = 5;
		ice->DiffuseSrvHeapIndex = 4;
		ice->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
		ice->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
		ice->Roughness = 1.0f;

		auto checkertile = std::make_unique<Material>();
		checkertile->Name = "checkertile";
		checkertile->MatCBIndex = 6;
		checkertile->DiffuseSrvHeapIndex = 5;
		checkertile->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
		checkertile->FresnelR0 = XMFLOAT3(0.07f, 0.07f, 0.07f);
		checkertile->Roughness = 0.3f;

		auto bricks = std::make_unique<Material>();
		bricks->Name = "bricks";
		bricks->MatCBIndex = 7;
		bricks->DiffuseSrvHeapIndex = 6;
		bricks->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
		bricks->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
		bricks->Roughness = 0.25f;

		auto yell = std::make_unique<Material>();
		yell->Name = "yell";
		yell->DiffuseAlbedo = { 1.0f, 1.0f, 0.0f, 0.6f };
		yell->FresnelR0 = { 0.05f, 0.05f, 0.05f };
		yell->Roughness = 0.0f;
		yell->MatCBIndex = 8;
		yell->DiffuseSrvHeapIndex = 3;

		XMFLOAT4 colors[8] = {
			XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f),
			XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f),
			XMFLOAT4(1.0f, 0.0f, 1.0f, 1.0f),
			XMFLOAT4(0.0f, 1.0f, 1.0f, 1.0f),
			XMFLOAT4(1.0f, 1.0f, 0.0f, 1.0f),
			XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
			XMFLOAT4(0.0f, 0.0f, 0.1f, 1.0f)
		};
		
		UINT CBIndex = 9;
		for (int i = 0; i < 8; ++i) {
			auto mirros = std::make_unique<Material>();

			std::string mirrorString = "mirror" + std::to_string(i + 1);
			mirros->Name = mirrorString;
			mirros->MatCBIndex = CBIndex++;
			mirros->DiffuseSrvHeapIndex = 3;
			mirros->DiffuseAlbedo = colors[i];
			mirros->FresnelR0 = XMFLOAT3(0.98f, 0.97f, 0.95f);
			mirros->Roughness = 0.1f;

			m_Materials[mirrorString] = std::move(mirros);
		}

		// 17 ->

		auto planeW = std::make_unique<Material>();
		planeW->Name = "planeWall1";
		planeW->DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
		planeW->FresnelR0 = { 0.01f, 0.01f, 0.01f };
		planeW->Roughness = 0.125f;
		planeW->MatCBIndex = 17;
		planeW->DiffuseSrvHeapIndex = 8;
		planeW->NormalSrvHeapIndex = 9;

		auto planeW2 = std::make_unique<Material>();
		planeW2->Name = "planeWall2";
		planeW2->DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
		planeW2->FresnelR0 = { 0.01f, 0.01f, 0.01f };
		planeW2->Roughness = 0.125f;
		planeW2->MatCBIndex = 18;
		planeW2->DiffuseSrvHeapIndex = 8;
		planeW2->NormalSrvHeapIndex = -1;

		m_Materials["grass"] = std::move(grass);
		m_Materials["water"] = std::move(water);
		m_Materials["wirefence"] = std::move(wirefence);
		m_Materials["white1x1"] = std::move(white1x1);
		m_Materials["ice"] = std::move(ice);
		m_Materials["checkertile"] = std::move(checkertile);
		m_Materials["bricks"] = std::move(bricks);
		m_Materials["yell"] = std::move(yell);
		m_Materials["mirror0"] = std::move(mirror0);
		m_Materials["planeWall1"] = std::move(planeW);
		m_Materials["planeWall2"] = std::move(planeW2);
	}

	void D3DApp::BuildShapeGeometry() {
		GeometryGenerator geoGen;
		GeometryGenerator::MeshData grid = geoGen.CreateGrid(20, 20, 40, 40);

		std::vector<Vertex> vertices(grid.Vertices.size());
		std::vector<std::uint16_t> indices = grid.GetIndices16();

		XMFLOAT3 _maxVec(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		XMFLOAT3 _minVec(FLT_MAX, FLT_MAX, FLT_MAX);

		XMVECTOR maxVec = XMLoadFloat3(&_maxVec);
		XMVECTOR minVec = XMLoadFloat3(&_minVec);

		for (size_t i = 0; i < grid.Vertices.size(); ++i) {
			vertices[i].Pos = grid.Vertices[i].Position;
			vertices[i].Normal = grid.Vertices[i].Normal;
			vertices[i].TexC = grid.Vertices[i].TexC;
			vertices[i].Tangent = grid.Vertices[i].TangentU;

			maxVec = XMVectorMax(XMLoadFloat3(&vertices[i].Pos), maxVec);
			minVec = XMVectorMax(XMLoadFloat3(&vertices[i].Pos), minVec);
		}

		UINT verticesSize = sizeof(Vertex) * (UINT)vertices.size();
		UINT indicesSize = sizeof(std::uint16_t) * (UINT)indices.size();

		auto meshGeo = std::make_unique<MeshGeometry>();
		D3DCreateBlob(verticesSize, &meshGeo->CPUVertexBuffer);
		D3DCreateBlob(indicesSize, &meshGeo->CPUIndexBuffer);
		meshGeo->GPUVertexBuffer = VertexBuffer::CreateDefaultBuffer(m_Device.Get(), m_CommandList.Get(),
			vertices.data(), verticesSize, meshGeo->GPUVertexUploader);
		meshGeo->GPUIndexBuffer = VertexBuffer::CreateDefaultBuffer(m_Device.Get(), m_CommandList.Get(),
			indices.data(), indicesSize, meshGeo->GPUIndexUploader);

		meshGeo->IndexBufferByteSize = indicesSize;
		meshGeo->IndexFormat = DXGI_FORMAT_R16_UINT;
		meshGeo->VertexByteStride = sizeof(Vertex);
		meshGeo->VertexBufferByteSize = verticesSize;
		meshGeo->Name = "planeGeo";

		SubMeshGeometry subMeshGeo;
		subMeshGeo.IndexCount = (UINT)indices.size();
		subMeshGeo.BaseVertexLocation = 0;
		subMeshGeo.StartIndexLocation = 0;

		BoundingBox bBox;
		XMStoreFloat3(&bBox.Extents, 0.5f * (maxVec - minVec));
		XMStoreFloat3(&bBox.Center, 0.5f * (maxVec + minVec));

		meshGeo->DrawArgs["grid"] = subMeshGeo;
		m_DrawArgs[meshGeo->Name] = std::move(meshGeo);
	}

	void D3DApp::BuildPlaneGeometry() {
		GeometryGenerator geoGen;
		GeometryGenerator::MeshData sphere = geoGen.CreateSphere(0.5f, 20, 20);

		std::vector<Vertex> vertices(sphere.Vertices.size());
		std::vector<std::uint16_t> indices = sphere.GetIndices16();

		XMFLOAT3 _maxVec(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		XMFLOAT3 _minVec(FLT_MAX, FLT_MAX, FLT_MAX);

		XMVECTOR maxVec = XMLoadFloat3(&_maxVec);
		XMVECTOR minVec = XMLoadFloat3(&_minVec);

		for (size_t i = 0; i < sphere.Vertices.size(); ++i) {
			vertices[i].Pos = sphere.Vertices[i].Position;
			vertices[i].Normal = sphere.Vertices[i].Normal;
			vertices[i].TexC = sphere.Vertices[i].TexC;

			maxVec = XMVectorMax(XMLoadFloat3(&vertices[i].Pos), maxVec);
			minVec = XMVectorMax(XMLoadFloat3(&vertices[i].Pos), minVec);
		}

		UINT verticesSize = sizeof(Vertex) * (UINT)vertices.size();
		UINT indicesSize = sizeof(std::uint16_t) * (UINT)indices.size();

		auto meshGeo = std::make_unique<MeshGeometry>();
		D3DCreateBlob(verticesSize, &meshGeo->CPUVertexBuffer);
		D3DCreateBlob(indicesSize, &meshGeo->CPUIndexBuffer);
		meshGeo->GPUVertexBuffer = VertexBuffer::CreateDefaultBuffer(m_Device.Get(), m_CommandList.Get(),
			vertices.data(), verticesSize, meshGeo->GPUVertexUploader);
		meshGeo->GPUIndexBuffer = VertexBuffer::CreateDefaultBuffer(m_Device.Get(), m_CommandList.Get(),
			indices.data(), indicesSize, meshGeo->GPUIndexUploader);

		meshGeo->IndexBufferByteSize = indicesSize;
		meshGeo->IndexFormat = DXGI_FORMAT_R16_UINT;
		meshGeo->VertexByteStride = sizeof(Vertex);
		meshGeo->VertexBufferByteSize = verticesSize;
		meshGeo->Name = "shapeGeo";

		SubMeshGeometry subMeshGeo;
		subMeshGeo.IndexCount = (UINT)indices.size();
		subMeshGeo.BaseVertexLocation = 0;
		subMeshGeo.StartIndexLocation = 0;

		BoundingBox bBox;
		XMStoreFloat3(&bBox.Extents, 0.5f * (maxVec - minVec));
		XMStoreFloat3(&bBox.Center, 0.5f * (maxVec + minVec));

		meshGeo->DrawArgs["sphere"] = subMeshGeo;
		m_DrawArgs[meshGeo->Name] = std::move(meshGeo);
	}

	void D3DApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& rItem) {
		auto currInstanceBuf = m_CurrFrameResource->m_InstCB->Resource();
		UINT objSize = VertexBuffer::CalcConstantBufferSize(sizeof(InstanceConstants));

		for (size_t i = 0; i < rItem.size(); ++i) {
			auto ri = rItem[i];

			if (ri->Visible == false) continue;

			cmdList->IASetVertexBuffers(0, 1, &My_unmove(ri->Geo->VertexBufferView()));
			cmdList->IASetIndexBuffer(&My_unmove(ri->Geo->IndexBufferView()));
			cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

			cmdList->SetGraphicsRootShaderResourceView(0, currInstanceBuf->GetGPUVirtualAddress() + objSize * ri->InstanceCount);

			cmdList->DrawIndexedInstanced(ri->IndexCount, 1,
				ri->StartIndexLocation, ri->BaseVertexLocation, 0);
		}
	}

	void D3DApp::DrawSceneToCubemap() {
		m_CommandList->RSSetScissorRects(1, &My_unmove(m_CubeRenderTarget->GetScissorRect()));
		m_CommandList->RSSetViewports(1, &My_unmove(m_CubeRenderTarget->GetViewport()));

		const UINT passCstSize = VertexBuffer::CalcConstantBufferSize(sizeof(PassConstants));

		auto rss = m_CubeRenderTarget->Resource();
		auto pass = m_CurrFrameResource->m_PassCB->Resource();

		m_CommandList->ResourceBarrier(1, &My_unmove(CD3DX12_RESOURCE_BARRIER::Transition(
			rss,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			D3D12_RESOURCE_STATE_RENDER_TARGET
		)));

		const FLOAT color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

		for (int i = 0; i < 6; ++i) {
			m_CommandList->OMSetRenderTargets(1, &My_unmove(m_CubeRenderTarget->RtvHandle(i)), true, &m_CubeMapDsv);

			m_CommandList->ClearDepthStencilView(m_CubeMapDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			m_CommandList->ClearRenderTargetView(My_unmove(m_CubeRenderTarget->RtvHandle(i)), color, 0, nullptr);

			D3D12_GPU_VIRTUAL_ADDRESS passGpuVirtualAddress = pass->GetGPUVirtualAddress() + (i + 1) * passCstSize;
			m_CommandList->SetGraphicsRootConstantBufferView(1, passGpuVirtualAddress);

			DrawRenderItems(m_CommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

			DrawRenderItems(m_CommandList.Get(), mRitemLayer[(int)RenderLayer::Skull]);
			
			m_CommandList->SetPipelineState(m_PSOs["sky"].Get());
			DrawRenderItems(m_CommandList.Get(), mRitemLayer[(int)RenderLayer::Sky]);

			m_CommandList->SetPipelineState(m_PSOs["opaque"].Get());
		}

		m_CommandList->ResourceBarrier(1, &My_unmove(CD3DX12_RESOURCE_BARRIER::Transition(
			rss,
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_GENERIC_READ
		)));
	}

	float D3DApp::GetHillsHeight(float x, float z) const {
		return 0.3f * (z * sinf(0.1f * x) + x * cosf(0.1f * z));
	}

	XMFLOAT3 D3DApp::GetHillsNormal(float x, float z) const {
		// n = (-df/dx, 1, -df/dz)
		XMFLOAT3 n(
			-0.03f * z * cosf(0.1f * x) - 0.3f * cosf(0.1f * z),
			1.0f,
			-0.3f * sinf(0.1f * x) + 0.03f * x * sinf(0.1f * z));

		XMVECTOR unitNormal = XMVector3Normalize(XMLoadFloat3(&n));
		XMStoreFloat3(&n, unitNormal);

		return n;
	}

	LRESULT D3DApp::MessageHandler(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
		if (m_D3DApp == nullptr) {
			return DefWindowProc(hwnd, msg, wp, lp);
		}

		switch (msg) {
		case WM_ACTIVATE:
			if (LOWORD(wp) == WA_INACTIVE) {
				*m_d3dSettings.appPaused = true;
				(*m_d3dSettings.gameTimer)->Stop();
			}
			else {
				*m_d3dSettings.appPaused = false;
				(*m_d3dSettings.gameTimer)->Start();
			}
			return 0;
		case WM_SIZE:
			m_d3dSettings.screenWidth = LOWORD(lp);
			m_d3dSettings.screenHeight = HIWORD(lp);

			if (wp == SIZE_MINIMIZED) {
				m_SizeMinimized = true;
				*m_d3dSettings.appPaused = true;
				m_SizeMaximized = false;
			}
			else if (wp == SIZE_MAXIMIZED) {
				m_SizeMinimized = false;
				*m_d3dSettings.appPaused = false;
				m_SizeMaximized = true;
				ResizeBuffer();
			}
			else if (wp == SIZE_RESTORED) {
				if (m_SizeMinimized) {
					*m_d3dSettings.appPaused = false;
					m_SizeMinimized = false;
					ResizeBuffer();
				}
				if (m_SizeMaximized) {
					*m_d3dSettings.appPaused = false;
					m_SizeMaximized = false;
					ResizeBuffer();
				}
			}
			else if (m_Resizing) {

			}
			else {
				ResizeBuffer();
			}
			return 0;
		case WM_ENTERSIZEMOVE:
			m_Resizing = true;
			*m_d3dSettings.appPaused = true;
			(*m_d3dSettings.gameTimer)->Stop();
			return 0;
		case WM_EXITSIZEMOVE:
			m_Resizing = false;
			*m_d3dSettings.appPaused = false;
			(*m_d3dSettings.gameTimer)->Start();
			ResizeBuffer();
			return 0;
		case WM_LBUTTONDOWN:
			return 0;
		case WM_RBUTTONDOWN:
			MouseDown(wp, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
			return 0;
		case WM_MBUTTONDOWN:
			return 0;
		case WM_LBUTTONUP:
			return 0;
		case WM_RBUTTONUP:
			MouseUp(wp, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
			return 0;
		case WM_MBUTTONUP:
			return 0;
		case WM_MOUSEMOVE:
			MouseMove(wp, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
			return 0;
		case WM_GETMINMAXINFO:
			((MINMAXINFO*)lp)->ptMinTrackSize.x = 200;
			((MINMAXINFO*)lp)->ptMinTrackSize.y = 200;
			return 0;
		case WM_MENUCHAR:
			return MAKELRESULT(0, MNC_CLOSE);
		case WM_KEYUP:
			if (wp == VK_ESCAPE) {
				PostQuitMessage(0);
			}
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		case WM_CLOSE:
			PostQuitMessage(0);
			return 0;
		}

		return DefWindowProc(hwnd, msg, wp, lp);
	}

	D3DApp* D3DApp::GetD3DApp() {
		return m_D3DApp;
	}

	void D3DApp::Pick(int sx, int sy) {
		XMFLOAT4X4 P = m_Camera.GetProjectionMatrix();
		XMMATRIX V = XMLoadFloat4x4(&My_unmove(m_Camera.GetViewMatrix()));
		XMMATRIX invView = XMMatrixInverse(&My_unmove(XMMatrixDeterminant(V)), V);

		float cx = ((2.0f * sx) / m_d3dSettings.screenWidth - 1.0f) / P(0, 0);
		float cy = ((-2.0f * sy) / m_d3dSettings.screenHeight + 1.0f) / P(1, 1);

		XMVECTOR rayOrigin = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
		XMVECTOR rayDir = XMVectorSet(cx, cy, 1.0f, 0.0f);

		m_PickedItem->Visible = false;

		for (auto& e : mRitemLayer[(int)RenderLayer::Skull]) {
			XMMATRIX W = XMLoadFloat4x4(&My_unmove(e->World));
			XMMATRIX invWorld = XMMatrixInverse(&My_unmove(XMMatrixDeterminant(W)), W);
			XMMATRIX toLocal = XMMatrixMultiply(invView, invWorld);

			rayOrigin = XMVector3TransformCoord(rayOrigin, toLocal);
			rayDir = XMVector3TransformNormal(rayDir, toLocal);
			rayDir = XMVector3Normalize(rayDir);

			float fMax = 0.0f;
			if (e->Bounds.Intersects(rayOrigin, rayDir, fMax)) {
				auto vertices = (Vertex*)e->Geo->CPUVertexBuffer->GetBufferPointer();
				auto indices = (uint32_t*)e->Geo->CPUIndexBuffer->GetBufferPointer();

				UINT triangleCount = e->IndexCount / 3;

				fMax = FLT_MAX;
				for (UINT i = 0; i < triangleCount; ++i) {
					UINT i1 = indices[i * 3 + 0];
					UINT i2 = indices[i * 3 + 1];
					UINT i3 = indices[i * 3 + 2];

					XMVECTOR v1 = XMLoadFloat3(&vertices[i1].Pos);
					XMVECTOR v2 = XMLoadFloat3(&vertices[i2].Pos);
					XMVECTOR v3 = XMLoadFloat3(&vertices[i3].Pos);

					float fm = 0.0f;
					if (DirectX::TriangleTests::Intersects(rayOrigin, rayDir, v1, v2, v3, fm)) {
						if (fm < fMax) {
							fMax = fm;

							UINT pickedTriangle = i;

							m_PickedItem->IndexCount = 3;
							m_PickedItem->Visible = true;
							m_PickedItem->BaseVertexLocation = 0;
							m_PickedItem->NumFramesDirty = gNumFrameResources;
							m_PickedItem->StartIndexLocation = pickedTriangle * 3;
							m_PickedItem->World = e->World;

							if (m_PickingFromAll) {
								m_PickedItem->IndexCount = e->IndexCount;
								m_PickedItem->StartIndexLocation = 0;
							}
						}
					}
				}
			}
		}
	}

	void D3DApp::GetBoundingBoxFromVertex(BoundingBox& bBox, const std::vector<Vertex>& vertices) {
		DirectX::XMFLOAT3 fvecMin(FLT_MAX, FLT_MAX, FLT_MAX);
		DirectX::XMFLOAT3 fvecMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);

		DirectX::XMVECTOR vecMin = DirectX::XMLoadFloat3(&fvecMin);
		DirectX::XMVECTOR vecMax = DirectX::XMLoadFloat3(&fvecMin);

		for (size_t i = 0; i < vertices.size(); ++i) {
			DirectX::XMVectorMin(DirectX::XMLoadFloat3(&vertices[i].Pos), vecMin);
			DirectX::XMVectorMax(DirectX::XMLoadFloat3(&vertices[i].Pos), vecMax);
		}

		DirectX::XMStoreFloat3(&bBox.Center, 0.5f * (vecMax + vecMin));
		DirectX::XMStoreFloat3(&bBox.Extents, 0.5f * (vecMax - vecMin));
	}

	void D3DApp::MouseDown(WPARAM btnState, int x, int y) {
		if (m_D3DApp != nullptr) {
			if ((btnState & MK_RBUTTON) != 0) {
				Pick(x, y);
			}

			m_LastMousePos.x = x;
			m_LastMousePos.y = y;

			SetCapture(m_d3dSettings.hwnd);
		}
	}

	void D3DApp::MouseUp(WPARAM btnState, int x, int y) {
		if (m_D3DApp != nullptr) {
			ReleaseCapture();
		}
	}

	void D3DApp::MouseMove(WPARAM btnState, int x, int y) {
		if ((btnState & MK_LBUTTON) != 0) {
			float dx = XMConvertToRadians(0.25f * static_cast<float>(x - m_LastMousePos.x));
			float dy = XMConvertToRadians(0.25f * static_cast<float>(y - m_LastMousePos.y));

			m_Camera.Pitch(dy);
			m_Camera.RotateY(dx);
		}
		else if ((btnState & MK_RBUTTON) != 0) {
			// float dx = 0.05f * static_cast<float>(x - m_LastMousePos.x);
			// float dy = 0.05f * static_cast<float>(y - m_LastMousePos.y);

			// r += dx - dy;
		}

		m_LastMousePos.x = x;
		m_LastMousePos.y = y;
	}
}

bool Mawi1e::D3DApp::m_isD3DSett = false;
Mawi1e::D3DApp* Mawi1e::D3DApp::m_D3DApp = nullptr;
LRESULT _stdcall WindowProcedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	return Mawi1e::D3DApp::GetD3DApp()->MessageHandler(hwnd, msg, wp, lp);
}
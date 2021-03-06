// Copyright (c) 2018-2019,  Zhirnov Andrey. For more information see 'LICENSE'

#include "SceneApp.h"
#include "scene/Loader/Assimp/AssimpLoader.h"
#include "scene/Loader/DevIL/DevILLoader.h"
#include "scene/Renderer/Prototype/RendererPrototype.h"
#include "scene/SceneManager/Simple/SimpleRayTracingScene.h"
#include "scene/SceneManager/DefaultSceneManager.h"
#include "pipeline_compiler/VPipelineCompiler.h"
#include "framework/Window/WindowGLFW.h"
#include "framework/Window/WindowSDL2.h"
#include "framework/Window/WindowSFML.h"
#include "stl/Algorithms/StringUtils.h"
#include "stl/Stream/FileStream.h"

#ifdef FG_STD_FILESYSTEM
#	include <filesystem>
	namespace fs = std::filesystem;
#endif

namespace FG
{

/*
=================================================
	constructor
=================================================
*/
	SceneApp::SceneApp () :
		_frameId{0}
	{
		_frameStat.lastUpdateTime = TimePoint_t::clock::now();
	}
	
/*
=================================================
	destructor
=================================================
*/
	SceneApp::~SceneApp ()
	{
		for (auto& cmd : _cmdBuffers) { cmd = null; }

		if ( _scene )
		{
			_scene->Destroy( _frameGraph );
			_scene = null;
		}

		if ( _renderTech )
		{
			_renderTech->Destroy();
			_renderTech = null;
		}

		if ( _frameGraph )
		{
			_frameGraph->ReleaseResource( _swapchainId );
			_frameGraph->Deinitialize();
			_frameGraph = null;
		}
	}
	
/*
=================================================
	Initialize
=================================================
*/
	bool SceneApp::Initialize ()
	{
		// will crash if it is not created as shared pointer
		ASSERT( shared_from_this() );

		CHECK_ERR( _CreateFrameGraph() );


		// upload resource data
		auto	cmdbuf = _frameGraph->Begin( CommandBufferDesc{ EQueueType::Graphics });
		{
			auto	renderer	= MakeShared<RendererPrototype>();
			auto	scene		= MakeShared<DefaultSceneManager>();
				
			CHECK_ERR( scene->Create( cmdbuf ));
			_scene = scene;

			CHECK_ERR( renderer->Create( _frameGraph ));
			_renderTech = renderer;

			CHECK_ERR( _scene->Add( _LoadScene2( cmdbuf )) );
		}
		CHECK_ERR( _frameGraph->Execute( cmdbuf ));
		CHECK_ERR( _frameGraph->Flush() );
		

		// create pipelines
		CHECK_ERR( _scene->Build( _renderTech ));
		CHECK_ERR( _frameGraph->Flush() );

		_camera.SetPosition({ -0.06f, 0.29f, 0.93f });

		_lastUpdateTime = TimePoint_t::clock::now();
		return true;
	}
	
/*
=================================================
	_LoadScene2
=================================================
*/
	SceneHierarchyPtr  SceneApp::_LoadScene2 (const CommandBuffer &cmdbuf) const
	{
		AssimpLoader			loader;
		AssimpLoader::Config	cfg;

		IntermScenePtr	temp_scene = loader.Load( cfg, FG_DATA_PATH "../_data/sponza/sponza.obj" );
		CHECK_ERR( temp_scene );
		
		DevILLoader		img_loader;
		CHECK_ERR( img_loader.Load( temp_scene, {FG_DATA_PATH "../_data/sponza"}, _scene->GetImageCache() ));
		
		IntermScenePtr	temp_scene2 = loader.Load( cfg, FG_DATA_PATH "../_data/bunny/bunny.obj" );
		CHECK_ERR( temp_scene2 );
		
		// setup material
		{
			auto&	mtr = temp_scene2->GetMaterials().begin()->first->EditSettings();
			mtr.albedo			= RGBA32f{ 0.8f, 0.8f, 1.0f, 1.0f };
			mtr.opticalDepth	= 2.6f;
			mtr.refraction		= 1.31f;	// ice
		}

		Transform	transform;
		transform.scale = 20.0f;
		temp_scene->Append( *temp_scene2, transform );

		transform.scale	= 0.01f;

		auto		hierarchy = MakeShared<SimpleRayTracingScene>();
		CHECK_ERR( hierarchy->Create( cmdbuf, temp_scene, _scene->GetImageCache(), transform ));

		return hierarchy;
	}

/*
=================================================
	_CreateFrameGraph
=================================================
*/
	bool SceneApp::_CreateFrameGraph ()
	{
		const uint2		wnd_size{ 1280, 960 };

		// initialize window
		{
			#if defined( FG_ENABLE_GLFW )
				_window.reset( new WindowGLFW() );

			#elif defined( FG_ENABLE_SDL2 )
				_window.reset( new WindowSDL2() );
			
			#elif defined(FG_ENABLE_SFML)
				_window.reset( new WindowSFML() );

			#else
			#	error Unknown window library!
			#endif

			_title = "Ray tracing";

			CHECK_ERR( _window->Create( wnd_size, _title ));
			_window->AddListener( this );
		}

		// initialize vulkan device
		{
			CHECK_ERR( _vulkan.Create( _window->GetVulkanSurface(), _title, "FrameGraph", VK_API_VERSION_1_1,
									   "RTX",
									   {},
									   VulkanDevice::GetRecomendedInstanceLayers(),
									   VulkanDevice::GetRecomendedInstanceExtensions(),
									   VulkanDevice::GetAllDeviceExtensions()
									));
			_vulkan.CreateDebugUtilsCallback( DebugUtilsMessageSeverity_All );
		}
		
		// setup device info
		VulkanDeviceInfo					vulkan_info;
		IFrameGraph::SwapchainCreateInfo_t	swapchain_info;
		{
			vulkan_info.instance		= BitCast<InstanceVk_t>( _vulkan.GetVkInstance() );
			vulkan_info.physicalDevice	= BitCast<PhysicalDeviceVk_t>( _vulkan.GetVkPhysicalDevice() );
			vulkan_info.device			= BitCast<DeviceVk_t>( _vulkan.GetVkDevice() );
			
			VulkanSwapchainCreateInfo	swapchain_ci;
			swapchain_ci.surface		= BitCast<SurfaceVk_t>( _vulkan.GetVkSurface() );
			swapchain_ci.surfaceSize	= _window->GetSize();
			swapchain_ci.presentModes.push_back( BitCast<PresentModeVk_t>(VK_PRESENT_MODE_MAILBOX_KHR) );
			//swapchain_ci.presentModes.push_back( BitCast<PresentModeVk_t>(VK_PRESENT_MODE_FIFO_KHR) );	// enable vsync
			swapchain_info				= swapchain_ci;

			for (auto& q : _vulkan.GetVkQuues())
			{
				VulkanDeviceInfo::QueueInfo	qi;
				qi.handle		= BitCast<QueueVk_t>( q.handle );
				qi.familyFlags	= BitCast<QueueFlagsVk_t>( q.flags );
				qi.familyIndex	= q.familyIndex;
				qi.priority		= q.priority;
				qi.debugName	= "";

				vulkan_info.queues.push_back( qi );
			}
		}

		// initialize framegraph
		{
			_frameGraph = IFrameGraph::CreateFrameGraph( vulkan_info );
			CHECK_ERR( _frameGraph );

			_swapchainId = _frameGraph->CreateSwapchain( swapchain_info );
			CHECK_ERR( _swapchainId );

			_frameGraph->SetShaderDebugCallback([this] (auto name, auto, auto, auto output) { _OnShaderTraceReady(name, output); });
		}

		// add glsl pipeline compiler
		{
			auto	compiler = MakeShared<VPipelineCompiler>( vulkan_info.physicalDevice, vulkan_info.device );
			compiler->SetCompilationFlags( EShaderCompilationFlags::AutoMapLocations |
										   EShaderCompilationFlags::Quiet			 |
										   EShaderCompilationFlags::ParseAnnoations );

			_frameGraph->AddPipelineCompiler( compiler );
		}
		
		// setup debug output
#		ifdef FG_STD_FILESYSTEM
		{
			fs::path	path{ FG_DATA_PATH "_debug_output" };
		
			if ( fs::exists( path ) )
				fs::remove_all( path );
		
			CHECK( fs::create_directory( path ));

			_debugOutputPath = path.string();
		}
#		endif	// FG_STD_FILESYSTEM

		return true;
	}

/*
=================================================
	Update
=================================================
*/
	bool SceneApp::Update ()
	{
		if ( not _window->Update() )
			return false;

		// wait frame-1 for double buffering
		_frameGraph->Wait({ _cmdBuffers[_frameId] });

		_scene->Draw({ shared_from_this() });
		_frameGraph->Flush();

		_UpdateFrameStat();

		++_frameId;
		return true;
	}
	
/*
=================================================
	OnResize
=================================================
*/
	void SceneApp::OnResize (const uint2 &size)
	{
		if ( Any( size == uint2(0) ))
			return;

		VulkanSwapchainCreateInfo	swapchain_info;
		swapchain_info.surface		= BitCast<SurfaceVk_t>( _vulkan.GetVkSurface() );
		swapchain_info.surfaceSize  = size;
		
		_swapchainId = _frameGraph->CreateSwapchain( swapchain_info, _swapchainId.Release() );
		CHECK_FATAL( _swapchainId );
		
		const vec2	view_size	{ size.x, size.y };
		_camera.SetPerspective( _fovY, view_size.x / view_size.y, _viewRange.x, _viewRange.y );
	}
	
/*
=================================================
	OnKey
=================================================
*/
	void SceneApp::OnKey (StringView key, EKeyAction action)
	{
		if ( action != EKeyAction::Up )
		{
			// forward/backward
			if ( key == "W" )			_positionDelta.x += 1.0f;	else
			if ( key == "S" )			_positionDelta.x -= 1.0f;

			// left/right
			if ( key == "D" )			_positionDelta.y -= 1.0f;	else
			if ( key == "A" )			_positionDelta.y += 1.0f;

			// up/down
			if ( key == "V" )			_positionDelta.z += 1.0f;	else
			if ( key == "C" )			_positionDelta.z -= 1.0f;

			// rotate up/down
			if ( key == "arrow up" )	_mouseDelta.y -= 0.01f;		else
			if ( key == "arrow down" )	_mouseDelta.y += 0.01f;

			// rotate left/right
			if ( key == "arrow right" )	_mouseDelta.x += 0.01f;		else
			if ( key == "arrow left" )	_mouseDelta.x -= 0.01f;
		}

		if ( action == EKeyAction::Down )
		{
			if ( key == "escape" and _window )	_window->Quit();
		}

		if ( key == "left mb" )			_mousePressed = (action != EKeyAction::Up);
	}
	
/*
=================================================
	OnMouseMove
=================================================
*/
	void SceneApp::OnMouseMove (const float2 &pos)
	{
		if ( _mousePressed )
		{
			vec2	delta = vec2{pos.x, pos.y} - _lastMousePos;
			_mouseDelta   += delta * 0.01f;
		}
		_lastMousePos = vec2{pos.x, pos.y};
	}

/*
=================================================
	Prepare
=================================================
*/
	void SceneApp::Prepare (ScenePreRender &preRender)
	{
		const auto	time	= TimePoint_t::clock::now();
		const auto	dt		= std::chrono::duration_cast<SecondsF>( time - _lastUpdateTime ).count();
		_lastUpdateTime = time;
		
		const uint2	wnd_size	= _window->GetSize();
		const vec2	view_size	{ wnd_size.x, wnd_size.y };
		//const vec2	view_size	{ 3840.0f, 2160.0f };

		_camera.SetPerspective( _fovY, view_size.x / view_size.y, _viewRange.x, _viewRange.y );
		_camera.Rotate( -_mouseDelta.x, _mouseDelta.y );

		if ( length2( _positionDelta ) > 0.01f ) {
			_positionDelta = normalize(_positionDelta) * _velocity * dt;
			_camera.Move2( _positionDelta );
		}

		LayerBits	layers;
		layers[uint(ERenderLayer::RayTracing)] = true;

		preRender.AddCamera( _camera.GetCamera(), view_size, _viewRange, DetailLevelRange{}, ECameraType::Main, layers, shared_from_this() );

		// reset
		_positionDelta	= vec3{0.0f};
		_mouseDelta		= vec2{0.0f};
	}
	
/*
=================================================
	Draw
=================================================
*/
	void SceneApp::Draw (RenderQueue &) const
	{
	}
	
/*
=================================================
	AfterRender
=================================================
*/
	void SceneApp::AfterRender (const CommandBuffer &cmdbuf, Present &&task)
	{
		FG_UNUSED( cmdbuf->AddTask( task.SetSwapchain( _swapchainId ) ));

		_cmdBuffers[_frameId] = cmdbuf;
	}
	
/*
=================================================
	_UpdateFrameStat
=================================================
*/
	void SceneApp::_UpdateFrameStat ()
	{
		using namespace std::chrono;

		++_frameStat.frameCounter;

		TimePoint_t		now			= TimePoint_t::clock::now();
		int64_t			duration	= duration_cast<milliseconds>(now - _frameStat.lastUpdateTime).count();

		IFrameGraph::Statistics		stat;
		_frameGraph->GetStatistics( OUT stat );
		_frameStat.gpuTimeSum += stat.renderer.gpuTime;
		_frameStat.cpuTimeSum += stat.renderer.cpuTime;

		if ( duration > _frameStat.UpdateIntervalMillis )
		{
			uint		fps_value	= uint(float(_frameStat.frameCounter) / float(duration) * 1000.0f + 0.5f);
			Nanoseconds	gpu_time	= _frameStat.gpuTimeSum / _frameStat.frameCounter;
			Nanoseconds	cpu_time	= _frameStat.cpuTimeSum / _frameStat.frameCounter;

			_frameStat.lastUpdateTime	= now;
			_frameStat.gpuTimeSum		= Default;
			_frameStat.cpuTimeSum		= Default;
			_frameStat.frameCounter		= 0;

			_window->SetTitle( String(_title) << " [FPS: " << ToString(fps_value) <<
							   ", GPU: " << ToString(gpu_time) <<
							   ", CPU: " << ToString(cpu_time) << ']' );
		}
	}
	
/*
=================================================
	_OnShaderTraceReady
=================================================
*/
	void SceneApp::_OnShaderTraceReady (StringView name, ArrayView<String> output) const
	{
	#	ifdef FG_STD_FILESYSTEM
		const auto	IsExists = [] (StringView path) { return fs::exists(fs::path{ path }); };
	#	else
		// TODO
	#	endif

		String	fname;

		for (auto& str : output)
		{
			for (uint index = 0; index < 100; ++index)
			{
				fname = String(_debugOutputPath) << '/' << name << '_' << ToString(index) << ".glsl_dbg";

				if ( IsExists( fname ) )
					continue;

				FileWStream		file{ fname };
				
				if ( file.IsOpen() )
					CHECK( file.Write( str ));
				
				FG_LOGI( "Shader trace saved to '"s << fname << "'" );
				break;
			}
		}
	
		//CHECK(!"shader trace is ready");
	}

}	// FG

/*
=================================================
	main
=================================================
*/
int main ()
{
	using namespace FG;
	{
		auto	scene = MakeShared<SceneApp>();

		CHECK_ERR( scene->Initialize(), -1 );

		for (; scene->Update();) {}
	}
	return 0;
}

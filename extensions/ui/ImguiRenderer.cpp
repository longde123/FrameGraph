// Copyright (c) 2018-2019,  Zhirnov Andrey. For more information see 'LICENSE'

#include "ImguiRenderer.h"
#include "imgui_internal.h"

namespace FG
{

/*
=================================================
	constructor
=================================================
*/
	ImguiRenderer::ImguiRenderer ()
	{
	}
	
/*
=================================================
	Initialize
=================================================
*/
	bool ImguiRenderer::Initialize (const FrameGraph &fg, ImGuiContext *ctx)
	{
		CHECK_ERR( ctx );

		_context = ctx;

		CHECK_ERR( _CreatePipeline( fg ));
		CHECK_ERR( _CreateSampler( fg ));
		
		// initialize font atlas
		{
			uint8_t*	pixels;
			int			width, height;
			_context->IO.Fonts->GetTexDataAsRGBA32( OUT &pixels, OUT &width, OUT &height );
		}
		return true;
	}
	
/*
=================================================
	Deinitialize
=================================================
*/
	void ImguiRenderer::Deinitialize (const FrameGraph &fg)
	{
		fg->ReleaseResource( INOUT _fontTexture );
		fg->ReleaseResource( INOUT _fontSampler );
		fg->ReleaseResource( INOUT _pipeline );
		fg->ReleaseResource( INOUT _vertexBuffer );
		fg->ReleaseResource( INOUT _indexBuffer );
		fg->ReleaseResource( INOUT _uniformBuffer );

		_context = null;
	}
	
/*
=================================================
	Draw
=================================================
*/
	Task  ImguiRenderer::Draw (const CommandBuffer &cmdbuf, LogicalPassID passId, ArrayView<Task> dependencies)
	{
		CHECK_ERR( cmdbuf and _context and _context->DrawData.Valid );

		ImDrawData&		draw_data = _context->DrawData;
		ASSERT( draw_data.TotalVtxCount > 0 );

		SubmitRenderPass	submit {passId};

		submit.DependsOn( _CreateFontTexture( cmdbuf ));
		submit.DependsOn( _RecreateBuffers( cmdbuf ));
		submit.DependsOn( _UpdateUniformBuffer( cmdbuf ));

		for (auto dep : dependencies) {
			submit.DependsOn( dep );
		}

		VertexInputState	vert_input;
		vert_input.Bind( VertexBufferID(), SizeOf<ImDrawVert> );
		vert_input.Add( VertexID("aPos"),   EVertexType::Float2,  OffsetOf( &ImDrawVert::pos ) );
		vert_input.Add( VertexID("aUV"),    EVertexType::Float2,  OffsetOf( &ImDrawVert::uv  ) );
		vert_input.Add( VertexID("aColor"), EVertexType::UByte4_Norm, OffsetOf( &ImDrawVert::col ) );

		uint	idx_offset	= 0;
		uint	vtx_offset	= 0;

		_resources.BindBuffer( UniformID("uPushConstant"), _uniformBuffer );
		_resources.BindTexture( UniformID("sTexture"), _fontTexture, _fontSampler );
		
		for (int i = 0; i < draw_data.CmdListsCount; ++i)
		{
			const ImDrawList &	cmd_list	= *draw_data.CmdLists[i];

			for (int j = 0; j < cmd_list.CmdBuffer.Size; ++j)
			{
				const ImDrawCmd &	cmd = cmd_list.CmdBuffer[j];

				if ( cmd.UserCallback )
				{
					cmd.UserCallback( &cmd_list, &cmd );
				}
				else
				{
					RectI	scissor;
					scissor.left	= int(cmd.ClipRect.x + 0.5f);
					scissor.top		= int(cmd.ClipRect.y + 0.5f);
					scissor.right	= int(cmd.ClipRect.z + 0.5f);
					scissor.bottom	= int(cmd.ClipRect.w + 0.5f);

					cmdbuf->AddTask( passId, DrawIndexed{}
									.SetPipeline( _pipeline ).AddResources( DescriptorSetID{"0"}, &_resources )
									.AddBuffer( VertexBufferID(), _vertexBuffer ).SetVertexInput( vert_input ).SetTopology( EPrimitive::TriangleList )
									.SetIndexBuffer( _indexBuffer, 0_b, EIndex::UShort )
									.Draw( cmd.ElemCount, 1, idx_offset, int(vtx_offset), 0 ).AddScissor( scissor ));
				}
				idx_offset += cmd.ElemCount;
			}

			vtx_offset += cmd_list.VtxBuffer.Size;
		}

		return cmdbuf->AddTask( submit );
	}

/*
=================================================
	_CreatePipeline
=================================================
*/
	bool ImguiRenderer::_CreatePipeline (const FrameGraph &fg)
	{
		using namespace std::string_literals;

		GraphicsPipelineDesc	desc;
		
		desc.AddShader( EShader::Vertex, EShaderLangFormat::VKSL_100, "main", R"#(
			#version 450 core
			layout(location = 0) in vec2 aPos;
			layout(location = 1) in vec2 aUV;
			layout(location = 2) in vec4 aColor;

			//layout(push_constant) uniform uPushConstant {
			layout(set=0, binding=1, std140) uniform uPushConstant {
				vec2 uScale;
				vec2 uTranslate;
			} pc;

			out gl_PerVertex{
				vec4 gl_Position;
			};

			layout(location = 0) out struct{
				vec4 Color;
				vec2 UV;
			} Out;

			void main()
			{
				Out.Color = aColor;
				Out.UV = aUV;
				gl_Position = vec4(aPos*pc.uScale+pc.uTranslate, 0, 1);
			})#"s );
		
		desc.AddShader( EShader::Fragment, EShaderLangFormat::VKSL_100, "main", R"#(
			#version 450 core
			layout(location = 0) out vec4 out_Color0;

			layout(set=0, binding=0) uniform sampler2D sTexture;

			layout(location = 0) in struct{
				vec4 Color;
				vec2 UV;
			} In;

			void main()
			{
				out_Color0 = In.Color * texture(sTexture, In.UV.st);
			})#"s );

		_pipeline = fg->CreatePipeline( desc );
		CHECK_ERR( _pipeline );

		CHECK_ERR( fg->InitPipelineResources( _pipeline, DescriptorSetID("0"), OUT _resources ));
		return true;
	}
	
/*
=================================================
	_CreateSampler
=================================================
*/
	bool  ImguiRenderer::_CreateSampler (const FrameGraph &fg)
	{
		SamplerDesc		desc;
		desc.magFilter		= EFilter::Linear;
		desc.minFilter		= EFilter::Linear;
		desc.mipmapMode		= EMipmapFilter::Linear;
		desc.addressMode	= { EAddressMode::Repeat };
		desc.minLod			= -1000.0f;
		desc.maxLod			= 1000.0f;

		_fontSampler = fg->CreateSampler( desc );
		CHECK_ERR( _fontSampler );
		return true;
	}

/*
=================================================
	_CreateFontTexture
=================================================
*/
	Task  ImguiRenderer::_CreateFontTexture (const CommandBuffer &cmdbuf)
	{
		if ( _fontTexture )
			return null;

		uint8_t*	pixels;
		int			width, height;

		_context->IO.Fonts->GetTexDataAsRGBA32( OUT &pixels, OUT &width, OUT &height );

		size_t		upload_size = width * height * 4 * sizeof(char);

		_fontTexture = cmdbuf->GetFrameGraph()
						->CreateImage( ImageDesc{ EImage::Tex2D, uint3{uint(width), uint(height), 1},
													EPixelFormat::RGBA8_UNorm, EImageUsage::Sampled | EImageUsage::TransferDst },
										Default, "UI.FontTexture" );
		CHECK_ERR( _fontTexture );

		return cmdbuf->AddTask( UpdateImage{}.SetImage( _fontTexture ).SetData( pixels, upload_size, uint2{int2{ width, height }} ));
	}
	
/*
=================================================
	_UpdateUniformBuffer
=================================================
*/
	Task  ImguiRenderer::_UpdateUniformBuffer (const CommandBuffer &cmdbuf)
	{
		ImDrawData &	draw_data = _context->DrawData;

		if ( not _uniformBuffer )
		{
			_uniformBuffer = cmdbuf->GetFrameGraph()
								->CreateBuffer( BufferDesc{ 16_b, EBufferUsage::Uniform | EBufferUsage::TransferDst },
												Default, "UI.UniformBuffer" );
			CHECK_ERR( _uniformBuffer );
		}
		
		float4		pc_data;
		// scale:
		pc_data[0] = 2.0f / (draw_data.DisplaySize.x * _context->IO.DisplayFramebufferScale.x);
		pc_data[1] = 2.0f / (draw_data.DisplaySize.y * _context->IO.DisplayFramebufferScale.y);
		// transform:
		pc_data[2] = -1.0f - draw_data.DisplayPos.x * pc_data[0];
		pc_data[3] = -1.0f - draw_data.DisplayPos.y * pc_data[1];

		return cmdbuf->AddTask( UpdateBuffer{}.SetBuffer( _uniformBuffer ).AddData( &pc_data, 1 ));
	}

/*
=================================================
	_RecreateBuffers
=================================================
*/
	Task  ImguiRenderer::_RecreateBuffers (const CommandBuffer &cmdbuf)
	{
		FrameGraph		fg			= cmdbuf->GetFrameGraph();
		ImDrawData &	draw_data	= _context->DrawData;
		BytesU			vertex_size	= draw_data.TotalVtxCount * SizeOf<ImDrawVert>;
		BytesU			index_size	= draw_data.TotalIdxCount * SizeOf<ImDrawIdx>;

		if ( not _vertexBuffer or vertex_size > _vertexBufSize )
		{
			fg->ReleaseResource( INOUT _vertexBuffer );

			_vertexBufSize	= vertex_size;
			_vertexBuffer	= fg->CreateBuffer( BufferDesc{ vertex_size, EBufferUsage::TransferDst | EBufferUsage::Vertex },
											    Default, "UI.VertexBuffer" );
		}

		if ( not _indexBuffer or index_size > _indexBufSize )
		{
			fg->ReleaseResource( INOUT _indexBuffer );

			_indexBufSize	= index_size;
			_indexBuffer	= fg->CreateBuffer( BufferDesc{ index_size, EBufferUsage::TransferDst | EBufferUsage::Index },
												Default, "UI.IndexBuffer" );
		}

		BytesU	vb_offset;
		BytesU	ib_offset;
		
		Task	last_task;

		for (int i = 0; i < draw_data.CmdListsCount; ++i)
		{
			const ImDrawList &	cmd_list = *draw_data.CmdLists[i];
			
			last_task = cmdbuf->AddTask( UpdateBuffer{}.SetBuffer( _vertexBuffer ).AddData( cmd_list.VtxBuffer.Data, cmd_list.VtxBuffer.Size, vb_offset ).DependsOn( last_task ));
			last_task = cmdbuf->AddTask( UpdateBuffer{}.SetBuffer( _indexBuffer ).AddData( cmd_list.IdxBuffer.Data, cmd_list.IdxBuffer.Size, ib_offset ).DependsOn( last_task ));

			vb_offset += cmd_list.VtxBuffer.Size * SizeOf<ImDrawVert>;
			ib_offset += cmd_list.IdxBuffer.Size * SizeOf<ImDrawIdx>;
		}

		ASSERT( vertex_size == vb_offset );
		ASSERT( index_size == ib_offset );
		return last_task;
	}


}	// FG

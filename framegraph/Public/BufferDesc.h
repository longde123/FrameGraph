// Copyright (c) 2018-2019,  Zhirnov Andrey. For more information see 'LICENSE'

#pragma once

#include "framegraph/Public/ResourceEnums.h"

namespace FG
{

	//
	// Buffer Description
	//

	struct BufferDesc
	{
	// variables
		BytesU			size;
		EBufferUsage	usage		= Default;
		EQueueUsage		queues		= Default;
		//bool			isLogical	= false;
		bool			isExternal	= false;

	// methods
		BufferDesc () {}
		BufferDesc (BytesU size, EBufferUsage usage, EQueueUsage queues = Default) : size{size}, usage{usage}, queues{queues} {}
	};


}	// FG

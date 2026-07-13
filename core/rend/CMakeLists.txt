# Portions Copyright 2026 The Hollycast Authors

if(NOT LIBRETRO)
	target_sources(${PROJECT_NAME} PRIVATE
			postprocess/glsl_hlsl_translator.cpp
			postprocess/reshade_fx.cpp
			postprocess/reshade_fx.h
			postprocess/reshade_compiler.cpp
			postprocess/shader_chain.cpp
			postprocess/shader_chain.h
			postprocess/slang_frontend.cpp
			postprocess/slang_frontend.h)
endif()

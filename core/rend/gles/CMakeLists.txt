# Portions Copyright 2026 The Hollycast Authors

target_sources(${PROJECT_NAME} PRIVATE
        glcache.h
        gldraw.cpp
        gles.cpp
        gles.h
        gltex.cpp
        quad.cpp
        postprocess.cpp
        postprocess.h
        naomi2.cpp
        naomi2.h)

if(NOT LIBRETRO)
    target_sources(${PROJECT_NAME} PRIVATE
            gl_shader_chain.cpp
            gl_shader_chain.h
            opengl_driver.cpp
            opengl_driver.h)
endif()

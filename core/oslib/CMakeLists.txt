# Portions Copyright 2026 The Hollycast Authors

target_sources(${PROJECT_NAME} PRIVATE
        directory.cpp
        directory.h
        host_context.h
        i18n.cpp
        i18n.h
        oslib.h
        resources.cpp
        resources.h
        storage.cpp
        storage.h
        unwind_info.h
        virtmem.h)

if(NOT LIBRETRO)
    target_sources(${PROJECT_NAME} PRIVATE
            http_client.cpp
            http_client.h
            oslib.cpp
			cpu_usage.cpp
			cpu_usage.h
            gpu_memory.cpp
            gpu_memory.h
            process_memory.cpp
            process_memory.h)
endif()

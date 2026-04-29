target_sources(${PROJECT_NAME} PRIVATE
        sdl.cpp
        sdl.h
        sdl_gamepad.cpp
        sdl_gamepad.h
        sdl_keyboard.h
        sdl_keyboard_mac.h
        sdl_mappingparser.h)

if (USE_DREAMLINK_DEVICES)
    file(GLOB DREAMLINK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/dreamlink/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/dreamlink/*.h")
    target_sources(${PROJECT_NAME} PRIVATE ${DREAMLINK_SRC})
endif()

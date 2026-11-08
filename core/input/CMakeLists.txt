target_sources(${PROJECT_NAME} PRIVATE
        dreamlink/dreamlink.cpp
        dreamlink/dreamlink.h
        dreamlink/dreampotato.cpp
        dreamlink/dreampotato.h
        dreamlink/maplelink.cpp
        dreamlink/maplelink.h
        dreamlink/maplelinkregistry.cpp
        dreamlink/maplelinkregistry.h)
if (USE_DREAMLINK_DEVICES)
    target_sources(${PROJECT_NAME} PRIVATE
        dreamlink/dreampicoport.cpp
        dreamlink/dreampicoport.h
    )
endif()
if(NOT LIBRETRO)
    target_sources(${PROJECT_NAME} PRIVATE
            gamepad.h
            gamepad_device.cpp
            gamepad_device.h
            keyboard_device.h
            mapping.cpp
            mapping.h
            mouse.cpp
            mouse.h)
endif()

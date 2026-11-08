target_include_directories(${PROJECT_NAME} PRIVATE ./)
target_sources(${PROJECT_NAME} PRIVATE
			android_gamepad.h
			android_gamepad.cpp
			android_input.cpp
			android_keyboard.h
			android_locale.h
			android_storage.h
			Android.cpp
			dreamlink_android_gamepad.h
			dreamlink_android_gamepad.cpp
			dreampicoport_android_gamepad.h
			dreampicoport_android_gamepad.cpp
			http_client.h
			i18n.cpp
			jni_util.h
			swappygl.cpp
			swappyvk.cpp)

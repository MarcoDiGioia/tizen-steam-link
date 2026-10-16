add_library(tsl-core STATIC
        client.cpp
        gamepad.c
        identity.cpp
        keymap.cpp
        log.cpp
        session.cpp)
target_include_directories(tsl-core PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tsl-core PUBLIC ihslib)

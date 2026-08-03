cmake_minimum_required(VERSION 3.16)
project(Cluster VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ============================================
# Qt6 Library Path Configuration
# ============================================
if(NOT DEFINED Qt6_DIR)
    message(FATAL_ERROR "Qt6_DIR not set. Please provide it via cmake:\n"
            "  cmake -S . -B <build_dir> -DQt6_DIR=<path_to_libs>/lib/cmake/Qt6")
endif()
message(STATUS "Using Qt6_DIR: ${Qt6_DIR}")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick)
qt_standard_project_setup(REQUIRES 6.5)

# ─── QNX: strip -lrt and -lpthread from Qt's interface libs ───────────────
if(QNX)
    foreach(_qt_target IN ITEMS
            Qt6::Core Qt6::Gui Qt6::Qml Qt6::QmlModels
            Qt6::QmlWorkerScript Qt6::Network Qt6::Quick)
        if(TARGET ${_qt_target})
            get_target_property(_libs ${_qt_target} INTERFACE_LINK_LIBRARIES)
            if(_libs)
                list(REMOVE_ITEM _libs rt pthread)
                set_target_properties(${_qt_target} PROPERTIES
                    INTERFACE_LINK_LIBRARIES "${_libs}")
            endif()
        endif()
    endforeach()
endif()

qt_add_executable(appCluster
    main.cpp
)
qt_add_qml_module(appCluster
    URI Cluster
    QML_FILES
        Main.qml
    SOURCES
        cluster.h
        cluster.cpp
        SpiReader.h
        SpiReader.cpp
        AiResultReader.h
        AiResultReader.cpp
        motor_wire.h
    RESOURCES
        images.qrc
)

# ─── Force-disable QML AOT cache generation ────────────────────────────────
set_target_properties(appCluster PROPERTIES
    QT_QML_MODULE_NO_CACHEGEN ON
    QT_QMLCACHEGEN_DIRECT_CALLS OFF
)
set_source_files_properties(Main.qml PROPERTIES
    QT_QML_SKIP_CACHEGEN ON
)

set_target_properties(appCluster PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appCluster
    PRIVATE Qt6::Quick
)

target_include_directories(appCluster PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ============================================
# Deploy paths (derive from Qt6_DIR)
# ============================================
get_filename_component(QT6_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)
set(QT6_LIB_DIR "${QT6_PREFIX}/lib" CACHE PATH "Qt6 shared libraries")
set(QT6_QML_DIR "${QT6_PREFIX}/qml" CACHE PATH "Qt6 QML imports")
set(QT6_PLUGIN_DIR "${QT6_PREFIX}/plugins" CACHE PATH "Qt6 plugins directory")

set(QT_DEPLOY_DIR "${CMAKE_BINARY_DIR}/deploy" CACHE PATH "Deploy output dir")
set(DEPLOY_LIB_DIR "${QT_DEPLOY_DIR}/lib")
set(DEPLOY_QML_DIR "${QT_DEPLOY_DIR}/qml")
set(DEPLOY_PLUGIN_DIR "${QT_DEPLOY_DIR}/plugins")
set(DEPLOY_FONTS_DIR "${QT_DEPLOY_DIR}/lib/fonts")

set(QNX_TARGET_ARCH "gcc_ntoaarch64le" CACHE STRING "QNX qcc target variant")
set(QNX_TARGET "$ENV{QNX_TARGET}" CACHE PATH "QNX target sysroot")
set(QNX_ARCH "aarch64le" CACHE STRING "QNX target architecture")
set(QNX_LIB_DIR "${QNX_TARGET}/${QNX_ARCH}" CACHE PATH "QNX arch root directory")
set(FONT_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../qt6-qnx-libs/fonts" CACHE PATH "Directory containing font files")

# ============================================
# Runtime output configuration
# ============================================
set_target_properties(appCluster PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${QT_DEPLOY_DIR}"
    INSTALL_RPATH "$ORIGIN/../lib"
    BUILD_RPATH "${QT6_LIB_DIR}"
)

# ============================================
# Deploy: copy only needed Qt + system deps
# ============================================
add_custom_command(TARGET appCluster POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DBIN=$<TARGET_FILE:appCluster>
        -DPROJ_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DQT_LIB_DIR=${QT6_LIB_DIR}
        -DDEPLOY_LIB_DIR=${DEPLOY_LIB_DIR}
        -DQT_QML_DIR=${QT6_QML_DIR}
        -DDEPLOY_QML_DIR=${DEPLOY_QML_DIR}
        -DQT_PLUGIN_DIR=${QT6_PLUGIN_DIR}
        -DDEPLOY_PLUGIN_DIR=${DEPLOY_PLUGIN_DIR}
        -DDEPLOY_FONTS_DIR=${DEPLOY_FONTS_DIR}
        -DFONT_SOURCE_DIR=${FONT_SOURCE_DIR}
        -DQNX_LIB_DIR=${QNX_LIB_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/deploy_qt.cmake
    COMMENT "Deploying minimal Qt dependencies to ${QT_DEPLOY_DIR}"
)

# Generate a launcher
file(GENERATE
    OUTPUT ${QT_DEPLOY_DIR}/run.sh
    CONTENT
"#!/bin/sh
DIR=\"$(cd \"$(dirname \"$0\")\" && pwd)\"
export LD_LIBRARY_PATH=\"$DIR/lib:$LD_LIBRARY_PATH\"
export QML2_IMPORT_PATH=\"$DIR/qml\"
export QT_QPA_PLATFORM_PLUGIN_PATH=\"$DIR/plugins\"
export QT_QUICK_BACKEND=\"software\"
export QQNX_PHYSICAL_SCREEN_SIZE=\"\${QQNX_PHYSICAL_SCREEN_SIZE:-150,90}\"
export APP_WIDTH=\"\${APP_WIDTH:-1280}\"
export APP_HEIGHT=\"\${APP_HEIGHT:-720}\"
export FONTCONFIG_FILE=\"$DIR/lib/fonts/fonts.conf\"
exec \"$DIR/appCluster\" \"\$@\"
"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)

message(STATUS "Build complete!")
message(STATUS "Binary: ${QT_DEPLOY_DIR}/appCluster")
message(STATUS "Run with: ${QT_DEPLOY_DIR}/run.sh")

# ─── QNX-specific ─────────────────────────────────────────────────────────
if(QNX)
    target_compile_definitions(appCluster PRIVATE __QNXNTO__)
    target_compile_options(appCluster PRIVATE
        -V${QNX_TARGET_ARCH}
        -Wc,-std=c++17
    )
    target_link_libraries(appCluster PRIVATE
        -lsocket
    )
    install(TARGETS appCluster
        RUNTIME DESTINATION /apps/cluster
    )
else()
    install(TARGETS appCluster
        BUNDLE  DESTINATION .
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

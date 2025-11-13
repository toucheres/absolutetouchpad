cmake_minimum_required(VERSION 3.14)

if (WIN32)
    project(absolutetouchpad LANGUAGES CXX)
elseif(UNIX)
    project(absolutetouchpad)
endif()

#set(CMAKE_CONFIGURATION_TYPES "Release;RelWithDebInfo" CACHE STRING "" FORCE)

# #======================= INCLUSION OF Qt =======================#
# set(CMAKE_INCLUDE_CURRENT_DIR ON)
# set(CMAKE_AUTOMOC ON)
# set(CMAKE_AUTOUIC ON)
# set(CMAKE_AUTORCC ON)
# set(CMAKE_PREFIX_PATH $ENV{QTDIR})

find_package(Qt6Core REQUIRED)
find_package(Qt6Widgets REQUIRED)
find_package(Qt6Multimedia REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core5Compat)
#=================== INCLUSION OF Project Files ====================#
set(FORMS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/forms")
set(INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/head")
set(SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

include_directories(${FORMS_DIR})
include_directories(${INCLUDE_DIR})
include_directories(${SOURCE_DIR})

file(GLOB_RECURSE SOURCES
    "${FORMS_DIR}/*.ui"
    "${FORMS_DIR}/*.qrc"
    "${INCLUDE_DIR}/*.h"
    "${SOURCE_DIR}/*.cpp"
)

#=================== SETUP EXECTUABLE ====================#
# Enable debug logging on RELWITHDEBINFO configuration
set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS
    $<$<CONFIG:RELWITHDEBINFO>:QT_MESSAGELOGCONTEXT>
)

#${CMAKE_AUTOUIC_SEARCH_PATHS}可能默认为cmakelists同级目录
message("--------------------------")
message(STATUS ${CMAKE_AUTOUIC_SEARCH_PATHS})
message("--------------------------")

#!!!必须将包含.ui .qrc的目录写入
# Add the forms directory to the AUTOUIC search paths
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_AUTOUIC_SEARCH_PATHS} ${FORMS_DIR})

message("--------------------------")
message(STATUS ${CMAKE_AUTOUIC_SEARCH_PATHS})
message("--------------------------")

# Add the executable
if (WIN32) 
    # add_executable(absolutetouchpad WIN32 ${SOURCES}) #无控制台
    add_executable(absolutetouchpad ${SOURCES})
elseif(UNIX)
    add_executable(absolutetouchpad ${SOURCES})
endif()
# 建议使用set_target_properties来代替全局set
set_target_properties(absolutetouchpad PROPERTIES
    AUTOMOC ON    # 处理Q_OBJECT宏
    AUTOUIC ON    # 处理.ui文件
    AUTORCC ON    # 处理.qrc文件
)
# Add the target includes for MY_PROJECT 
target_include_directories(absolutetouchpad PRIVATE ${FORMS_DIR})
target_include_directories(absolutetouchpad PRIVATE ${INCLUDE_DIR})
target_include_directories(absolutetouchpad PRIVATE ${SOURCE_DIR})

#===================== LINKING LIBRARIES =======================#

target_link_libraries(absolutetouchpad Qt6::Widgets Qt6::Multimedia Qt6::Core5Compat)


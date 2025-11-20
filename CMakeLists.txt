# CMakeList.txt : Top-level CMake project file, do global configuration
# and include sub-projects here.
#
cmake_minimum_required (VERSION 3.8)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()
#set(Boost_COMPILER "-vc144")
#set(CMAKE_C_COMPILER_LAUNCHER ccache)
#set(CMAKE_CXX_COMPILER_LAUNCHER ccache)

project ("Test_Chat_Project")
set(GLOBAL_INCLUDE_DIR C:/includes)
set(PROJECT_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include) 
set(LINUX_PROJECT_INCLUDE_DIR /root/.vs/Test_Chat_Project/include)
set(LINUX_SRC_DIR /home/ross/.vs/Test_Chat_Project/src)
set(LINUX_GLOBAL_INCLUDE_DIR /usr/local/include) 
#set(CMAKE_VERBOSE_MAKEFILE ON)
        
# Include sub-projects.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux") 
add_subdirectory("Test_Chat_Server")
set(BOOST_ROOT "usr/local/include/boost_1_85_0")
elseif(WIN32)
add_subdirectory ("Test_Chat_Client") 
set(BOOST_ROOT "C:/includes/boost_1_89_0")
set(CPACK_GENERATOR "NSIS")
set(CPACK_PACKAGE_NAME "Test_Chat_Client")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Test_Chat_Client")
include(CPack)
endif()
#find_package(Boost REQUIRED COMPONENTS chrono)


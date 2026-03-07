# CMakeList.txt : CMake project for Test_Chat_Project, include source and define
# project specific logic here.
#

# Add source to this project's executable.
add_executable (Test_Chat_Client  "Chat_Client.cpp"
	"${GLOBAL_INCLUDE_DIR}/glad/glad.c"
	"imgui.cpp" 
	"imgui_impl_glfw.cpp"
	"imgui_impl_opengl3.cpp"
	"imgui_draw.cpp"
	"imgui_tables.cpp"
	"imgui_widgets.cpp"
	"imgui_demo.cpp"
	"imgui_stdlib.cpp"

)

set_target_properties(Test_Chat_Client PROPERTIES LINKER_LANGUAGE CXX)
message(STATUS "CLIENT PROJECT_INCLUDE_DIR = ${PROJECT_INCLUDE_DIR}")

target_include_directories(Test_Chat_Client PRIVATE 
"${GLOBAL_INCLUDE_DIR}/boost_1_89_0"
"${GLOBAL_INCLUDE_DIR}/glm"
"${GLOBAL_INCLUDE_DIR}/imgui"
"${GLOBAL_INCLUDE_DIR}/imgui/backends"
"${GLOBAL_INCLUDE_DIR}/imgui/misc/cpp"
"${GLOBAL_INCLUDE_DIR}/miniaudio"
#"${GLOBAL_INCLUDE_DIR}/kissfft"
"${GLOBAL_INCLUDE_DIR}/opus/include"
"${GLOBAL_INCLUDE_DIR}"
"${PROJECT_INCLUDE_DIR}"
"C:/Boost/include/boost-1_89/"
)

#target_compile_definitions(Test_Chat_Client PRIVATE OPENSSL_NO_DYNAMIC_ENGINE)
#add_library(kissfft STATIC ${GLOBAL_INCLUDE_DIR}/kissfft/kiss_fft.c)

set(OPUS_DEBUG_LIB "${GLOBAL_INCLUDE_DIR}/opus/build/Debug/opus.lib")
set(OPUS_RELEASE_LIB "${GLOBAL_INCLUDE_DIR}/opus/build/Release/opus.lib")
set(BOOST_LIB_DIR "${GLOBAL_INCLUDE_DIR}/boost_1_89_0/_build/stage/lib/Debug")

target_link_libraries(Test_Chat_Client PRIVATE
	$<$<CONFIG:DEBUG>:
		${BOOST_LIB_DIR}/libboost_thread-vc143-mt-gd-x64-1_89.lib>
	$<$<CONFIG:RELEASE>:
		${BOOST_LIB_DIR}/libboost_thread-vc143-mt-x64-1_89.lib>
	$<$<CONFIG:DEBUG>:
		${BOOST_LIB_DIR}/libboost_atomic-vc143-mt-gd-x64-1_89.lib>
	$<$<CONFIG:RELEASE>:
		${BOOST_LIB_DIR}/libboost_atomic-vc143-mt-x64-1_89.lib>
	$<$<CONFIG:DEBUG>:
		${BOOST_LIB_DIR}/libboost_chrono-vc143-mt-gd-x64-1_89.lib>
	$<$<CONFIG:RELEASE>:
		${BOOST_LIB_DIR}/libboost_chrono-vc143-mt-x64-1_89.lib>
	$<$<CONFIG:DEBUG>:
		${BOOST_LIB_DIR}/libboost_container-vc143-mt-gd-x64-1_89.lib>
	$<$<CONFIG:RELEASE>:
		${BOOST_LIB_DIR}/libboost_container-vc143-mt-x64-1_89.lib>
	$<$<CONFIG:DEBUG>:
		${BOOST_LIB_DIR}/libboost_date_time-vc143-mt-gd-x64-1_89.lib>
	$<$<CONFIG:RELEASE>:
		${BOOST_LIB_DIR}/libboost_date_time-vc143-mt-x64-1_89.lib>

	"${GLOBAL_INCLUDE_DIR}/boost_1_89_0/"
	"${GLOBAL_INCLUDE_DIR}/openssl/lib/VC/x64/MTd/libssl_static.lib"
	"${GLOBAL_INCLUDE_DIR}/openssl/lib/VC/x64/MTd/libcrypto_static.lib"
	$<IF:$<CONFIG:Debug>,${OPUS_DEBUG_LIB}, ${OPUS_RELEASE_LIB}>
	${GLOBAL_INCLUDE_DIR}/glfw/glfw3.lib
	Ws2_32.lib
	Crypt32.lib
	CryptUI.lib
)
# Force static CRT: /MT for Release, /MTd for Debug
#set_property(TARGET Test_Chat_Client PROPERTY
 # MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
#)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET Test_Chat_Client PROPERTY CXX_STANDARD 20)
endif()

# TODO: Add tests and install targets if needed.
install(TARGETS Test_Chat_Client DESTINATION bin)
install(FILES config.ini DESTINATION config)
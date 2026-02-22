add_executable(Test_Chat_Server Chat_Server.cpp)

set_target_properties(Test_Chat_Server PROPERTIES LINKER_LANGUAGE CXX)

message(STATUS "SERVER GLOBAL_LINUX_INCLUDE_DIR = ${LINUX_GLOBAL_INCLUDE_DIR}")
message(STATUS "SERVER PROJECT_LINUX_SOURCE_DIR = ${LINUX_PROJECT_INCLUDE_DIR}")
message(STATUS "PROJECT_SOURCE_DIR = ${PROJECT_SOURCE_DIR}/include")#PROJECT_SOURCE_DIR = /home/ross/.vs/Test_Chat_Project/include

target_include_directories(Test_Chat_Server PRIVATE
	"${LINUX_GLOBAL_INCLUDE_DIR}"
	"${LINUX_GLOBAL_INCLUDE_DIR}/ssl/include"
	"${LINUX_GLOBAL_INCLUDE_DIR}/miniaudio"
	#"${LINUX_SRC_DIR}/kissfft"
	"${LINUX_GLOBAL_INCLUDE_DIR}/opus/include"
    "${PROJECT_SOURCE_DIR}/include"
)
target_link_directories(Test_Chat_Server PRIVATE
	"${LINUX_GLOBAL_INCLUDE_DIR}/ssl/lib64"
)

#add_library(kissfft STATIC ${LINUX_SRC_DIR}/kissfft/kiss_fft.c)
#target_compile_definitions(kissfft PRIVATE KISS_FFT_USE_ALLOCA=1)

set(OPUS_DEBUG_LIB "${LINUX_GLOBAL_INCLUDE_DIR}/opus/build_debug/libopus.a")
set(OPUS_RELEASE_LIB "${LINUX_GLOBAL_INCLUDE_DIR}/opus/build_release/libopus.a")
set(SSL_DEBUG_LIB "${LINUX_GLOBAL_INCLUDE_DIR}/ssl/lib64/libssl.so")
set(SSL_RELEASE_LIB "${LINUX_GLOBAL_INCLUDE_DIR}/ssl/lib64/libssl.a")
set(CRYPTO_DEBUG_LIB "${LINUX_GLOBAL_INCLUDE_DIR}/ssl/lib64/libcrypto.so")
set(CRYPTO_RELEASE_LIB "${LINUX_GLOBAL_INCLUDE_DIR}/ssl/lib64/libcrypto.a")

set(BOOST_DEBUG_LIB_DIR "${LINUX_GLOBAL_INCLUDE_DIR}/boost_1_89_0/_build-shared/stage/lib")
set(BOOST_RELEASE_LIB_DIR "${LINUX_GLOBAL_INCLUDE_DIR}/boost_1_89_0/_build/stage/lib")

target_link_libraries(Test_Chat_Server PRIVATE
	#"${LINUX_GLOBAL_INCLUDE_DIR}/ssl/lib64/libssl.a"
	#"${LINUX_GLOBAL_INCLUDE_DIR}/ssl/lib64/libcrypto.a"
	$<IF:$<CONFIG:Debug>, ${SSL_DEBUG_LIB}, ${SSL_RELEASE_LIB}>
	$<IF:$<CONFIG:Debug>, ${CRYPTO_DEBUG_LIB}, ${CRYPTO_RELEASE_LIB}>
	$<IF:$<CONFIG:Debug>, ${OPUS_DEBUG_LIB}, ${OPUS_RELEASE_LIB}>
	$<$<CONFIG:DEBUG>:
		${BOOST_DEBUG_LIB_DIR}/libboost_thread.so>
	$<$<CONFIG:RELEASE>:
		${BOOST_RELEASE_LIB_DIR}/libboost_thread.a>
	$<$<CONFIG:DEBUG>:
		${BOOST_DEBUG_LIB_DIR}/libboost_atomic.so>
	$<$<CONFIG:RELEASE>:
		${BOOST_RELEASE_LIB_DIR}/libboost_atomic.a>
	$<$<CONFIG:DEBUG>:
		${BOOST_DEBUG_LIB_DIR}/libboost_chrono.so>
	$<$<CONFIG:RELEASE>:
		${BOOST_RELEASE_LIB_DIR}/libboost_chrono.a>
	$<$<CONFIG:DEBUG>:
		${BOOST_DEBUG_LIB_DIR}/libboost_container.so>
	$<$<CONFIG:RELEASE>:
		${BOOST_RELEASE_LIB_DIR}/libboost_container.a>
	$<$<CONFIG:DEBUG>:
		${BOOST_DEBUG_LIB_DIR}/libboost_date_time.so>
	$<$<CONFIG:RELEASE>:
		${BOOST_RELEASE_LIB_DIR}/libboost_date_time.a>
	#kissfft
)
if(CMAKE_VERSION VERSION_GREATER 3.12)
	set_property(TARGET Test_Chat_Server PROPERTY CXX_STANDARD 20)
	endif()
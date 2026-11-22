# libjuice 1.7.5 (https://github.com/paullouisageneau/libjuice, MPL-2.0): netplay's ICE connections through NAT.
add_library(pcsx2-juice STATIC
	include/juice/juice.h
	src/addr.c
	src/agent.c
	src/base64.c
	src/conn.c
	src/conn_mux.c
	src/conn_poll.c
	src/conn_thread.c
	src/const_time.c
	src/crc32.c
	src/hash.c
	src/hmac.c
	src/ice.c
	src/juice.c
	src/log.c
	src/random.c
	src/server.c
	src/stun.c
	src/tcp.c
	src/timestamp.c
	src/turn.c
	src/udp.c
)

target_compile_definitions(pcsx2-juice PUBLIC JUICE_STATIC PRIVATE NO_SERVER USE_NETTLE=0)
target_include_directories(pcsx2-juice PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/include/juice"
	"${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(pcsx2-juice PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

find_package(Threads REQUIRED)
target_link_libraries(pcsx2-juice PRIVATE Threads::Threads)
if(WIN32)
	target_link_libraries(pcsx2-juice PRIVATE ws2_32 bcrypt)
endif()

add_library(LibJuice::LibJuice ALIAS pcsx2-juice)

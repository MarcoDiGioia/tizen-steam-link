# ihslib built from the pinned source list. Its own CMakeLists.txt needs pkg-config, links
# the system SDL2 when it finds one and always builds the SDL HID library; none of that
# applies here. Threads come from src/platform/ihs_thread_pthread.c instead of SDL, and UDP
# from src/platform/ihs_udp_poll.c (no fcntl, poll-based timeouts) instead of
# platforms/ihs_udp_posix.c.
set(IHS_DIR ${ihslib_SOURCE_DIR})

add_library(ihslib STATIC
        ${IHS_DIR}/src/base.c
        ${IHS_DIR}/src/crc32.c
        ${IHS_DIR}/src/crc32c.c
        ${IHS_DIR}/src/ihs_arraylist.c
        ${IHS_DIR}/src/ihs_buffer.c
        ${IHS_DIR}/src/ihs_enumeration.c
        ${IHS_DIR}/src/ihs_enumeration_array.c
        ${IHS_DIR}/src/ihs_enumeration_ll.c
        ${IHS_DIR}/src/ihs_ip.c
        ${IHS_DIR}/src/ihs_queue.c
        ${IHS_DIR}/src/ihs_timer.c
        ${IHS_DIR}/src/client/authorization.c
        ${IHS_DIR}/src/client/authorization_kx.c
        ${IHS_DIR}/src/client/client.c
        ${IHS_DIR}/src/client/discovery.c
        ${IHS_DIR}/src/client/streaming.c
        ${IHS_DIR}/src/crypto/impl_mbedtls.c
        ${IHS_DIR}/src/hid/device.c
        ${IHS_DIR}/src/hid/manager.c
        ${IHS_DIR}/src/hid/provider.c
        ${IHS_DIR}/src/hid/report.c
        ${IHS_DIR}/src/platforms/ihs_ip_posix.c
        ${IHS_DIR}/src/protobuf/discovery.pb-c.c
        ${IHS_DIR}/src/protobuf/hiddevices.pb-c.c
        ${IHS_DIR}/src/protobuf/pb_utils.c
        ${IHS_DIR}/src/protobuf/remoteplay.pb-c.c
        ${IHS_DIR}/src/session/callbacks.c
        ${IHS_DIR}/src/session/frame.c
        ${IHS_DIR}/src/session/frame_crypto.c
        ${IHS_DIR}/src/session/frame_stats.c
        ${IHS_DIR}/src/session/packet.c
        ${IHS_DIR}/src/session/retransmission.c
        ${IHS_DIR}/src/session/session.c
        ${IHS_DIR}/src/session/window.c
        ${IHS_DIR}/src/session/channels/channel.c
        ${IHS_DIR}/src/session/channels/ch_control.c
        ${IHS_DIR}/src/session/channels/ch_control_audio.c
        ${IHS_DIR}/src/session/channels/ch_control_authentication.c
        ${IHS_DIR}/src/session/channels/ch_control_keepalive.c
        ${IHS_DIR}/src/session/channels/ch_control_microphone.c
        ${IHS_DIR}/src/session/channels/ch_control_negotiation.c
        ${IHS_DIR}/src/session/channels/ch_control_video.c
        ${IHS_DIR}/src/session/channels/ch_data.c
        ${IHS_DIR}/src/session/channels/ch_data_audio.c
        ${IHS_DIR}/src/session/channels/ch_data_microphone.c
        ${IHS_DIR}/src/session/channels/ch_discovery.c
        ${IHS_DIR}/src/session/channels/ch_stats.c
        ${IHS_DIR}/src/session/channels/control/control_cursor.c
        ${IHS_DIR}/src/session/channels/control/control_hid.c
        ${IHS_DIR}/src/session/channels/control/control_input_kbd.c
        ${IHS_DIR}/src/session/channels/control/control_input_mouse.c
        ${IHS_DIR}/src/session/channels/control/control_input_touch.c
        ${IHS_DIR}/src/session/channels/video/ch_data_video.c
        ${IHS_DIR}/src/session/channels/video/frame_h264.c
        ${IHS_DIR}/src/session/channels/video/frame_hevc.c
        ${IHS_DIR}/src/session/channels/video/partial_frames.c
        ${CMAKE_CURRENT_LIST_DIR}/../src/platform/ihs_thread_pthread.c
        ${CMAKE_CURRENT_LIST_DIR}/../src/platform/ihs_udp_poll.c)

target_include_directories(ihslib PUBLIC ${IHS_DIR}/include)
# Private headers are public to our own code on purpose: the unicast probe and the
# gamepad HID provider are written against ihslib internals (pinned commit).
target_include_directories(ihslib PUBLIC ${IHS_DIR}/src)
target_link_libraries(ihslib PUBLIC protobuf-c mbedcrypto)
if (NOT EMSCRIPTEN)
    target_link_libraries(ihslib PUBLIC Threads::Threads m)
endif ()
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ihslib PRIVATE -w)
endif ()

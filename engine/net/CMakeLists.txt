add_library(slash_net
    src/enet.cpp
    src/multiplayer.cpp
    src/peer.cpp
    src/websocket.cpp)
add_library(slash::net ALIAS slash_net)

target_include_directories(slash_net PUBLIC include)
target_link_libraries(slash_net PUBLIC slash::core slash::world)
slash_engine_target(slash_net)

# ENet everywhere but the web (cmake/Dependencies.cmake).
if(TARGET slash_enet)
    target_link_libraries(slash_net PRIVATE slash_enet)
    target_compile_definitions(slash_net PRIVATE SLASH_NET_ENET=1)
else()
    target_compile_definitions(slash_net PRIVATE SLASH_NET_ENET=0)
endif()
# WebSocket natively over wslay, ENet's sockets and Mbed TLS's SHA-1; on the web through the
# browser's WebSocket.
if(TARGET slash_wslay AND TARGET slash_enet AND TARGET slash_mbedtls)
    target_link_libraries(slash_net PRIVATE slash_wslay slash_mbedtls)
    target_compile_definitions(slash_net PRIVATE SLASH_NET_WEBSOCKET=1)
else()
    target_compile_definitions(slash_net PRIVATE SLASH_NET_WEBSOCKET=0)
endif()
if(WIN32)
    # ENet's header brings in Winsock's.
    target_compile_definitions(slash_net PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ── Protocol + proxy core (no Boost dependency) ──────────────────────────────
add_library(proxy_lib STATIC
  gsup/GsupCodec.cpp
  ipa/IpaCodec.cpp
  map/BerCodec.cpp
  map/MapCodec.cpp
  proxy/TransactionManager.cpp
  proxy/Converter.cpp
  proxy/Proxy.cpp
)

target_include_directories(proxy_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(proxy_lib PUBLIC Threads::Threads spdlog::spdlog)

# ── IPA transport (requires Boost.Asio) ──────────────────────────────────────
add_library(transport_lib STATIC
  transport/IpaSession.cpp
  transport/IpaServer.cpp
  transport/M3uaCodec.cpp
  transport/ScccpCodec.cpp
  transport/MapTransport.cpp
)

target_include_directories(transport_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(transport_lib
  PUBLIC  proxy_lib Boost::headers  # Boost headers are in public .hpp files
)

# Disable deprecation warnings from older Boost headers on macOS
target_compile_definitions(transport_lib PRIVATE
  BOOST_ASIO_NO_DEPRECATED
  _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
)

if(ENABLE_SCTP_TRANSPORT)
  target_compile_definitions(transport_lib PRIVATE ENABLE_SCTP_TRANSPORT)
endif()

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(gsup_map_proxy main.cpp)
target_link_libraries(gsup_map_proxy PRIVATE transport_lib)

add_executable(vanilla-pipe
    main.c
    wpa.c
        mdns.c
)



install(TARGETS vanilla-pipe)

target_include_directories(vanilla-pipe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/hostap/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/hostap/src/utils
    ${CMAKE_SOURCE_DIR}/lib
)

# Build WPA supplicant
add_custom_target(
    wpa_supplicant_configure
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/hostap/conf/wpa_supplicant.config" "${CMAKE_CURRENT_SOURCE_DIR}/hostap/wpa_supplicant/.config"
)
add_custom_target(
    wpa_supplicant_build
    COMMAND make -j$$(nproc) && cp wpa_supplicant wpa_supplicant_drc
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/hostap/wpa_supplicant"
    BYPRODUCTS "${CMAKE_CURRENT_SOURCE_DIR}/hostap/wpa_supplicant/wpa_supplicant_drc"
    USES_TERMINAL
    DEPENDS wpa_supplicant_configure
)
add_executable(wpa_supplicant IMPORTED)
add_dependencies(wpa_supplicant wpa_supplicant_build)
set_target_properties(wpa_supplicant PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/hostap/wpa_supplicant/wpa_supplicant_drc"
)
add_custom_command(
    TARGET wpa_supplicant_build POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/hostap/wpa_supplicant/wpa_supplicant_drc" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    DEPENDS wpa_supplicant_build
)

# Build client library
add_custom_target(
    wpa_client_build
    COMMAND make libwpa_client.so
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/hostap/wpa_supplicant"
    DEPENDS wpa_supplicant
    BYPRODUCTS "${CMAKE_CURRENT_SOURCE_DIR}/hostap/wpa_supplicant/libwpa_client.so"
)
add_library(wpa_client SHARED IMPORTED)
set_target_properties(wpa_client PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/hostap/wpa_supplicant/libwpa_client.so"
    IMPORTED_NO_SONAME true
)
add_dependencies(wpa_client wpa_client_build)
find_package(Avahi REQUIRED COMPONENTS client common)
# Link our library with the client library
target_link_libraries(vanilla-pipe PRIVATE
    wpa_client
    pthread
        ${APP_NAME_LC}::AvahiCommon
        ${APP_NAME_LC}::Avahi
)

# Copy files into build directory
#add_custom_command(
#    TARGET wpa_supplicant POST_BUILD
#    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/hostap/wpa_supplicant/wpa_supplicant" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/wpa_supplicant_drc"
#)

install(IMPORTED_RUNTIME_ARTIFACTS wpa_client wpa_supplicant)

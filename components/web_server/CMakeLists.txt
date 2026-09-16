idf_component_register(SRCS "web_server.cpp" INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}" REQUIRES api esp_http_server)
set(UI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../webui")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/web_assets.hpp"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/../../tools/build_webui.py" "${UI_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/web_assets.hpp"
    DEPENDS "${UI_DIR}/index.html" "${UI_DIR}/app.js" "${UI_DIR}/app.css" "${CMAKE_CURRENT_LIST_DIR}/../../tools/build_webui.py")
add_custom_target(web_assets DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/web_assets.hpp")
add_dependencies(${COMPONENT_LIB} web_assets)

# copy_if_missing.cmake -- copy SRC to DST only if DST does not already exist.
#
# Used to seed build\mgmp.json from mgmp.json.template without ever clobbering a
# config the user has since edited (game, net.role, net.control). Invoked as a
# POST_BUILD step because CMake has no built-in "copy unless present" command.
if(NOT EXISTS "${DST}")
    file(COPY_FILE "${SRC}" "${DST}")
    message(STATUS "mgmp: wrote default ${DST}")
endif()

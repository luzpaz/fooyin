function (fooyin_print_summary)
  message(STATUS "")
  message(STATUS "******** Fooyin Configuration ********")
  message(STATUS "General:")
  message(STATUS "  Fooyin version        : ${FOOYIN_VERSION}")
  message(STATUS "  CMake version         : ${CMAKE_VERSION}")
  message(STATUS "  CMake command         : ${CMAKE_COMMAND}")
  message(STATUS "  System                : ${CMAKE_SYSTEM_NAME}")
  message(STATUS "  C++ compiler          : ${CMAKE_CXX_COMPILER}")
  message(STATUS "  C++ compiler version  : ${CMAKE_CXX_COMPILER_VERSION}")
  message(STATUS "  CXX flags             : ${CMAKE_CXX_FLAGS}")
  message(STATUS "  Build type            : ${CMAKE_BUILD_TYPE}")
  message(STATUS "")
  message(STATUS "Install location:")
  message(STATUS "  Install prefix        : ${CMAKE_INSTALL_PREFIX}")
  message(STATUS "  Binary install dir    : ${CMAKE_INSTALL_PREFIX}/${BIN_INSTALL_DIR}")
  message(STATUS "  Library install dir   : ${CMAKE_INSTALL_PREFIX}/${LIB_INSTALL_DIR}")
  message(STATUS "  Plugin install dir    : ${CMAKE_INSTALL_PREFIX}/${FOOYIN_PLUGIN_INSTALL_DIR}")
  message(STATUS "")
  message(STATUS "Options:")
  message(STATUS "  BUILD_SHARED_LIBS     : ${BUILD_SHARED_LIBS}")
  message(STATUS "  BUILD_TESTING         : ${BUILD_TESTING}")
  message(STATUS "  BUILD_PLUGINS         : ${BUILD_PLUGINS}")
  message(STATUS "  BUILD_TRANSLATIONS    : ${BUILD_TRANSLATIONS}")
  message(STATUS "  BUILD_CCACHE          : ${BUILD_CCACHE}")
  message(STATUS "  BUILD_PCH             : ${BUILD_PCH}")
  message(STATUS "  BUILD_WERROR          : ${BUILD_WERROR}")
  message(STATUS "  FOOYIN_DEPLOY         : ${FOOYIN_DEPLOY}")
endfunction()

if (NOT TARGET PaRSEC::parsec)
  find_package(PaRSEC CONFIG QUIET COMPONENTS parsec HINTS ${PaRSEC_ROOT_DIR})
  if (TARGET PaRSEC::parsec)
    message(STATUS "Found PaRSEC: PaRSEC_CONFIG=${PaRSEC_CONFIG}")
  endif (TARGET PaRSEC::parsec)
endif (NOT TARGET PaRSEC::parsec)

if (NOT TARGET PaRSEC::parsec)

  # configure PaRSEC
  set(SUPPORT_FORTRAN OFF CACHE BOOL "Disable Fortran support in PaRSEC")
  set(CMAKE_CROSSCOMPILING OFF)
  set(CMAKE_SYSTEM_PROCESSOR ${CMAKE_HOST_SYSTEM_PROCESSOR})
  set(PARSEC_WITH_DEVEL_HEADERS ON CACHE BOOL "Install PaRSEC headers")
  set(BUILD_TOOLS OFF CACHE BOOL "Do not build PaRSEC tools")

  FetchContent_Declare(
          PARSEC
          #GIT_REPOSITORY      https://bitbucket.org/herault/parsec.git
          GIT_REPOSITORY      https://bitbucket.org/schuchart/parsec.git
          GIT_TAG             ${TTG_TRACKED_PARSEC_TAG}
  )
  FetchContent_MakeAvailable(PARSEC)
  FetchContent_GetProperties(PARSEC
          SOURCE_DIR PARSEC_SOURCE_DIR
          BINARY_DIR PARSEC_BINARY_DIR
          )

  # export parsec targets from the build tree for the same to be possible for madness targets
  export(EXPORT parsec-targets FILE "${PROJECT_BINARY_DIR}/parsec-targets.cmake")

  # set PaRSEC_CONFIG to the install location so that we know where to find it
  set(PaRSEC_CONFIG ${CMAKE_INSTALL_PREFIX}/share/cmake/parsec/PaRSECConfig.cmake)

endif(NOT TARGET PaRSEC::parsec)

# postcond check
if (NOT TARGET PaRSEC::parsec)
  message(FATAL_ERROR "FindOrFetchPARSEC could not make PaRSEC::parsec target available")
endif(NOT TARGET PaRSEC::parsec)

if (NOT TARGET Boost::boost)
  find_package(Boost ${TTG_TRACKED_BOOST_VERSION} QUIET CONFIG OPTIONAL_COMPONENTS serialization)
endif(NOT TARGET Boost::boost)

if (TARGET Boost::boost)
  set(_msg "Found Boost at ${Boost_CONFIG}")
  if (TARGET Boost::serialization)
    list(APPEND _msg " includes Boost::serialization")
  endif(TARGET Boost::serialization)
  message(STATUS "${_msg}")
else (TARGET Boost::boost)

  FetchContent_Declare(
          CMAKEBOOST
          GIT_REPOSITORY      https://github.com/Orphis/boost-cmake
  )
  FetchContent_MakeAvailable(CMAKEBOOST)
  FetchContent_GetProperties(CMAKEBOOST
          SOURCE_DIR CMAKEBOOST_SOURCE_DIR
          BINARY_DIR CMAKEBOOST_BINARY_DIR
          )

  # current boost-cmake/master does not install boost correctly, so warn that installed TTG will not be usable
  # boost-cmake/install_rules https://github.com/Orphis/boost-cmake/pull/45 is supposed to fix it but is inactive
  message(WARNING "Building Boost from source makes TTG unusable from the install location! Install Boost using package manager or manually and reconfigure/reinstall TTG to fix this")

  if (TARGET Boost::serialization AND TARGET Boost_serialization)
    install(TARGETS Boost_serialization EXPORT boost)
    export(EXPORT boost
           FILE "${PROJECT_BINARY_DIR}/boost-targets.cmake")
    install(EXPORT boost
            FILE "boost-targets.cmake"
            DESTINATION "${CMAKE_INSTALL_CMAKEDIR}"
            COMPONENT boost-libs)
  endif()

endif(TARGET Boost::boost)

# postcond check
if (NOT TARGET Boost::boost)
  message(FATAL_ERROR "FindOrFetchBoost could not make Boost::boost target available")
endif(NOT TARGET Boost::boost)

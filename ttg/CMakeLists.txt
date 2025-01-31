include (AddTTGLibrary)

########################
####### Runtime-neutral
########################
set(ttg-util-headers
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/backtrace.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/bug.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/demangle.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/dot.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/future.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/hash.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/macro.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/meta.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/print.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/span.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/trace.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/tree.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/version.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/void.h
    )
set(ttg-base-headers
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/base/keymap.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/base/op.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/base/terminal.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/base/world.h
    )
set(ttg-impl-headers
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/broadcast.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/edge.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/execution.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/func.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/fwd.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/impl_selector.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/op.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/reduce.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/runtimes.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/terminal.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/traverse.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/world.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/wrap.h
    )
set(ttg-headers
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg.h
    )
set(ttg-sources
        ${ttg-headers}
        ${ttg-impl-headers}
        ${ttg-base-headers}
        ${ttg-util-headers}
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/backtrace.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/bug.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/util/version.cc
    )
# optional dependencies
set(ttg-deps Boost::boost)
if (TARGET TTG_Libunwind)
  list(APPEND ttg-deps TTG_Libunwind)
endif(TARGET TTG_Libunwind)

add_ttg_library(ttg "${ttg-sources}" PUBLIC_HEADER "${ttg-headers};${ttg-impl-headers};${ttg-base-headers};${ttg-util-headers}" LINK_LIBRARIES "${ttg-deps}")

########################
####### Serialization
########################
set(ttg-serialization-headers
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/backends.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/buffer_archive.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/buffer_archive.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/data_descriptor.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/splitmd_data_descriptor.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/stream.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/traits.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/backends/boost.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/backends/boost/archive.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/backends/cereal.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/backends/madness.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/std/allocator.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/std/array.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/std/list.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/std/pair.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/std/tuple.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/std/variant.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/std/vector.h
   )
set(ttg-serialization-sources
        ${ttg-serialization-headers}
        ${CMAKE_CURRENT_SOURCE_DIR}/ttg/serialization/buffer_archive.cpp
        )
if (TARGET MADworld)
  list(APPEND ttg-serialization-deps MADworld)
  list(APPEND ttg-serialization-compile-definitions TTG_SERIALIZATION_SUPPORTS_MADNESS=1)
endif(TARGET MADworld)
if (TARGET Boost::serialization)
  list(APPEND ttg-serialization-deps Boost::serialization)
  list(APPEND ttg-serialization-compile-definitions TTG_SERIALIZATION_SUPPORTS_BOOST=1)
endif (TARGET Boost::serialization)
if (TARGET cereal::cereal)
  list(APPEND ttg-serialization-deps cereal::cereal)
  list(APPEND ttg-serialization-compile-definitions TTG_SERIALIZATION_SUPPORTS_CEREAL=1)
endif (TARGET cereal::cereal)

add_ttg_library(ttg-serialization
                "${ttg-serialization-sources}"
                PUBLIC_HEADER "${ttg-serialization-headers}"
                LINK_LIBRARIES "${ttg-serialization-deps}"
                COMPILE_DEFINITIONS "${ttg-serialization-compile-definitions}")
# make madness-only serialization target
if (TARGET MADworld)
  add_ttg_library(ttg-serialization-madness
          "${ttg-serialization-sources}"
          PUBLIC_HEADER "${ttg-serialization-headers}"
          LINK_LIBRARIES "MADworld"
          COMPILE_DEFINITIONS "TTG_SERIALIZATION_SUPPORTS_MADNESS=1")
endif(TARGET MADworld)
# make boost-only serialization target
if (TARGET Boost::serialization)
  add_ttg_library(ttg-serialization-boost
          "${ttg-serialization-sources}"
          PUBLIC_HEADER "${ttg-serialization-headers}"
          LINK_LIBRARIES "Boost::serialization"
          COMPILE_DEFINITIONS "TTG_SERIALIZATION_SUPPORTS_BOOST=1")
endif(TARGET Boost::serialization)
# make cereal-only serialization target
if (TARGET cereal::cereal)
  add_ttg_library(ttg-serialization-cereal
          "${ttg-serialization-sources}"
          PUBLIC_HEADER "${ttg-serialization-headers}"
          LINK_LIBRARIES "cereal::cereal"
          COMPILE_DEFINITIONS "TTG_SERIALIZATION_SUPPORTS_CEREAL=1")
endif(TARGET cereal::cereal)

#########################
####### MADNESS-specific
#########################
if (TARGET MADworld)
  set(ttg-mad-headers
          ${CMAKE_CURRENT_SOURCE_DIR}/ttg/madness/fwd.h
          ${CMAKE_CURRENT_SOURCE_DIR}/ttg/madness/import.h
          ${CMAKE_CURRENT_SOURCE_DIR}/ttg/madness/ttg.h
          ${CMAKE_CURRENT_SOURCE_DIR}/ttg/madness/watch.h)
  # N.B. ttg-mad can use MADNESS serialization only
  add_ttg_library(ttg-mad "${ttg-mad-headers}" PUBLIC_HEADER "${ttg-mad-headers}" LINK_LIBRARIES "ttg;Boost::boost;MADworld;ttg-serialization-madness")
endif(TARGET MADworld)

########################
####### PaRSEC-specific
########################
if (TARGET PaRSEC::parsec)
  set(ttg-parsec-headers
          ${CMAKE_CURRENT_SOURCE_DIR}/ttg/parsec/fwd.h
          ${CMAKE_CURRENT_SOURCE_DIR}/ttg/parsec/import.h
          ${CMAKE_CURRENT_SOURCE_DIR}/ttg/parsec/ttg.h
          ${CMAKE_CURRENT_SOURCE_DIR}/ttg/parsec/ttg_data_copy.h
          )
  set(ttg-parsec-sources
          ${CMAKE_CURRENT_SOURCE_DIR}/ttg/parsec/parsec_ttg.cc
          ${ttg-parsec-headers})
  find_package(MPI)
  set(ttg-parsec-deps "ttg;Boost::boost;MPI::MPI_CXX;PaRSEC::parsec")
  # parsec depends on TTG's serialization layer since it does not provide its own
  if (TTG_PARSEC_USE_BOOST_SERIALIZATION AND TARGET ttg-serialization-boost)
    list(APPEND ttg-parsec-deps ttg-serialization-boost)
  else()
    list(APPEND ttg-parsec-deps ttg-serialization)
  endif()
  add_ttg_library(ttg-parsec "${ttg-parsec-sources}" PUBLIC_HEADER "${ttg-parsec-headers}" LINK_LIBRARIES "${ttg-parsec-deps}")
endif(TARGET PaRSEC::parsec)

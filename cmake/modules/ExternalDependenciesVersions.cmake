# for each dependency track both current and previous id (the variable for the latter must contain PREVIOUS)
# to be able to auto-update them

# need Boost.CallableTraits (header only, part of Boost 1.66 released in Dec 2017) for wrap.h to work
set(TTG_TRACKED_BOOST_VERSION 1.66)
set(TTG_TRACKED_MADNESS_TAG 977bd48793fd609641983a8ba4fa18c8b9882e4f)
set(TTG_TRACKED_PARSEC_TAG 1ca1d5f53bc7417395a12b8e6140a072cc0098ac)
# The pinned archive's headers, not host libraries. libtorrent 2.1 needs
# Boost.Asio and Boost.JSON; both are compiled into its own static library.
if(NOT EXISTS "${OPENCE_BOOST_SOURCE}/boost/version.hpp")
    message(FATAL_ERROR "Pinned Boost headers are missing")
endif()
set(Boost_FOUND TRUE)
set(Boost_VERSION 108800)
set(Boost_MAJOR_VERSION 1)
set(Boost_MINOR_VERSION 88)
set(Boost_SUBMINOR_VERSION 0)
set(Boost_INCLUDE_DIRS "${OPENCE_BOOST_SOURCE}")
if(NOT TARGET Boost::headers)
    add_library(Boost::headers INTERFACE IMPORTED GLOBAL)
    set_target_properties(Boost::headers PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${OPENCE_BOOST_SOURCE}")
endif()

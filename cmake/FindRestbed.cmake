if(NOT Restbed_FOUND)
    find_path (Restbed_INCLUDE_DIR restbed
               HINTS
               "/usr/include"
               "/usr/local/include"
               "/opt/local/include")
    find_library(Restbed_LIBRARY restbed
                 HINTS ${Restbed_ROOT_DIR} PATH_SUFFIXES lib)
    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(Restbed DEFAULT_MSG Restbed_LIBRARY Restbed_INCLUDE_DIR)

    pkg_search_module(libssl REQUIRED libssl)
    pkg_search_module(libcrypto REQUIRED libcrypto)

    if (Restbed_INCLUDE_DIR)
        set(Restbed_FOUND TRUE)
        set(Restbed_LIBRARIES ${Restbed_LIBRARY} ${libssl_LINK_LIBRARIES} ${libcrypto_LINK_LIBRARIES})
        set(Restbed_INCLUDE_DIRS ${Restbed_INCLUDE_DIR} ${libssl_INCLUDE_DIR} ${libcrypto_INCLUDE_DIR})
    endif()
endif()

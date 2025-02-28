include_directories( ${ENGINE_COMMON_INCLUDES} ${ENGINE_BLOCKCACHE_INCLUDE} ${ENGINE_PRIMPROC_INCLUDE} )

if (WITH_UNITTESTS)
    set(EXTERNAL_INSTALL_LOCATION ${CMAKE_BINARY_DIR}/external)
    ExternalProject_Add(googletest
        GIT_REPOSITORY https://github.com/google/googletest
        GIT_TAG release-1.11.0
        CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${EXTERNAL_INSTALL_LOCATION} -DBUILD_SHARED_LIBS=ON
    )

    include_directories(${EXTERNAL_INSTALL_LOCATION}/include)
    # lib64 for RPM-based distros
    link_directories(${EXTERNAL_INSTALL_LOCATION}/lib ${EXTERNAL_INSTALL_LOCATION}/lib64)
    set(GTEST_LIBRARIES gtest gtest_main pthread)
    include(GoogleTest)

    #GoogleTest tests
    add_executable(rowgroup_tests rowgroup-tests.cpp)
    add_dependencies(rowgroup_tests googletest)
    target_link_libraries(rowgroup_tests ${ENGINE_LDFLAGS} ${GTEST_LIBRARIES} ${ENGINE_EXEC_LIBS} ${MARIADB_CLIENT_LIBS})
    gtest_discover_tests(rowgroup_tests TEST_PREFIX columnstore:)

    add_executable(mcs_decimal_tests mcs_decimal-tests.cpp)
    add_dependencies(mcs_decimal_tests googletest)
    target_link_libraries(mcs_decimal_tests ${ENGINE_LDFLAGS} ${GTEST_LIBRARIES} ${ENGINE_EXEC_LIBS} ${MARIADB_CLIENT_LIBS})
    gtest_discover_tests(mcs_decimal_tests TEST_PREFIX columnstore:)

    add_executable(dataconvert_tests dataconvert-tests.cpp)
    add_dependencies(dataconvert_tests googletest)
    target_link_libraries(dataconvert_tests ${ENGINE_LDFLAGS} ${GTEST_LIBRARIES} ${ENGINE_EXEC_LIBS} ${MARIADB_CLIENT_LIBS})
    gtest_discover_tests(dataconvert_tests TEST_PREFIX columnstore:)

    add_executable(rebuild_em_tests rebuild-em-tests.cpp)
    add_dependencies(rebuild_em_tests googletest)
    target_link_libraries(rebuild_em_tests ${ENGINE_LDFLAGS} ${GTEST_LIBRARIES} ${MARIADB_CLIENT_LIBS} ${ENGINE_WRITE_LIBS})
    gtest_discover_tests(rebuild_em_tests TEST_PREFIX columnstore:)

    add_executable(compression_tests compression-tests.cpp)
    add_dependencies(compression_tests googletest)
    target_link_libraries(compression_tests ${ENGINE_LDFLAGS} ${GTEST_LIBRARIES} ${MARIADB_CLIENT_LIBS} ${ENGINE_WRITE_LIBS})
    gtest_discover_tests(compression_tests TEST_PREFIX columnstore:)

    add_executable(column_scan_filter_tests primitives_column_scan_and_filter.cpp)
    target_compile_options(column_scan_filter_tests PRIVATE -Wno-error -Wno-sign-compare)
    add_dependencies(column_scan_filter_tests googletest)
    target_link_libraries(column_scan_filter_tests ${ENGINE_LDFLAGS} ${MARIADB_CLIENT_LIBS} ${ENGINE_WRITE_LIBS} ${GTEST_LIBRARIES} processor dbbc)
    gtest_discover_tests(column_scan_filter_tests TEST_PREFIX columnstore:)

    add_executable(simd_processors simd_processors.cpp)
    add_dependencies(simd_processors googletest)
    target_link_libraries(simd_processors ${ENGINE_LDFLAGS} ${MARIADB_CLIENT_LIBS} ${ENGINE_WRITE_LIBS} ${GTEST_LIBRARIES} processor dbbc)
    gtest_discover_tests(simd_processors TEST_PREFIX columnstore:)

    # CPPUNIT TESTS
    add_executable(we_shared_components_tests shared_components_tests.cpp)
    add_dependencies(we_shared_components_tests loggingcpp)
    target_link_libraries(we_shared_components_tests ${ENGINE_LDFLAGS} ${MARIADB_CLIENT_LIBS} ${ENGINE_WRITE_LIBS} cppunit)
    #Fix Test and uncomment
    #add_test(NAME columnstore:we_shared_components_tests, COMMAND we_shared_components_tests)

    add_executable(comparators_tests comparators-tests.cpp)
    target_link_libraries(comparators_tests ${ENGINE_LDFLAGS} ${MARIADB_CLIENT_LIBS} ${ENGINE_WRITE_LIBS} ${CPPUNIT_LIBRARIES} cppunit)
    add_test(NAME columnstore:comparators_tests, COMMAND comparators_tests)

    # standalone EM routines test
    # add_executable(brm_em_standalone brm-em-standalone.cpp)
    # target_link_libraries(brm_em_standalone ${ENGINE_LDFLAGS} ${MARIADB_CLIENT_LIBS} ${ENGINE_WRITE_LIBS} ${CPPUNIT_LIBRARIES} cppunit)
    # install(TARGETS brm_em_standalone DESTINATION ${ENGINE_BINDIR} COMPONENT columnstore-engine)

endif()

# Saving this as the example of the microbench
#if (WITH_MICROBENCHMARKS AND (NOT CMAKE_BUILD_TYPE STREQUAL "debug"))
#    find_package(benchmark REQUIRED)
#    add_executable(primitives_scan_bench primitives_scan_bench.cpp)
#    target_include_directories(primitives_scan_bench PUBLIC ${ENGINE_COMMON_INCLUDES} ${ENGINE_BLOCKCACHE_INCLUDE} ${ENGINE_PRIMPROC_INCLUDE} )
#    target_link_libraries(primitives_scan_bench ${ENGINE_LDFLAGS} ${MARIADB_CLIENT_LIBS} ${ENGINE_WRITE_LIBS} ${GTEST_LIBRARIES} processor dbbc benchmark::benchmark)
#    install(TARGETS primitives_scan_bench DESTINATION ${ENGINE_BINDIR} COMPONENT columnstore-engine)
#endif()


get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests ${_TEST_NAME}_tests_main.cpp
    sla_print_tests.cpp
    sla_test_utils.hpp sla_test_utils.cpp
    sla_supptgen_tests.cpp
    sla_supports_slicing.cpp
    sla_raycast_tests.cpp
    sla_parabola_tests.cpp
    sla_voronoi_graph_tests.cpp
    sla_vectorUtils_tests.cpp
    sla_lineUtils_tests.cpp
    sla_supptreeutils_tests.cpp
    sla_archive_readwrite_tests.cpp
    sla_zcorrection_tests.cpp
    sla_print_job_tests.cpp
    sla_ctb_tests.cpp
    sla_tinymaker_tests.cpp
    sla_goo_tests.cpp
    sla_resin_catalogue_tests.cpp
    sla_imagezip_tests.cpp
    sla_anycubic_tests.cpp
    sla_cxdlp_tests.cpp
    sla_svgx_tests.cpp
    sla_chituboxzip_tests.cpp
    sla_resin_material_tests.cpp
    sla_hollow_tests.cpp
    sla_support_points_tests.cpp)
target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)

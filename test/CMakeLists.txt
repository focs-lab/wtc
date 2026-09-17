configure_lit_site_cfg(
        ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
        ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
        MAIN_CONFIG
        ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
)

set(COSYNTH_TEST_DEPENDS
        cosynth-opt
        )

add_lit_testsuite(check-cosynth "Running the cosynth regression tests"
        ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${COSYNTH_TEST_DEPENDS}
        )
set_target_properties(check-cosynth PROPERTIES FOLDER "Tests")

add_lit_testsuites(COSYNTH ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS ${COSYNTH_TEST_DEPENDS})

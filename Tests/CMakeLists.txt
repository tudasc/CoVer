set(COVER_INSTALLED_BINARIES_DIR "${CMAKE_INSTALL_PREFIX}/bin")
configure_file(lit.cfg.in ${CMAKE_CURRENT_LIST_DIR}/lit.cfg @ONLY)

function(add_cover_test TEST_NAME)
  add_test(NAME "C::${TEST_NAME}" COMMAND lit --verbose ${CMAKE_CURRENT_LIST_DIR}/c/${TEST_NAME}.c)
  add_test(NAME "Fortran::${TEST_NAME}" COMMAND lit --verbose ${CMAKE_CURRENT_LIST_DIR}/fort/${TEST_NAME}.F90)
endfunction()

add_cover_test(Correct-Minimal)
add_cover_test(Correct-P2P)
add_cover_test(Correct-P2PStackBuf)

add_cover_test(PostCall-MissingFinalize)
add_cover_test(PreCall-MissingInit)
add_cover_test(Release-DataRace)
add_cover_test(Param-InvalidComm)

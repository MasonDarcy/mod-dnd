# mod-dnd's own build settings (included by modules/CMakeLists.txt, in the "modules" target's scope).
# DndTest.cpp - the self-test jig and every test in Catalogue() - outgrew MSVC's object file section limit (C1128):
# compile it with extended object support.
if(MSVC)
  set_source_files_properties("${CMAKE_SOURCE_DIR}/modules/mod-dnd/src/DndTest.cpp"
    PROPERTIES COMPILE_OPTIONS "/bigobj")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(ATK REQUIRED IMPORTED_TARGET atk)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)

ladybird_test(TestLadybirdAtkObject.cpp Accessibility/Linux LIBS LibWebView LibUnicode)
target_sources(TestLadybirdAtkObject PRIVATE
    ${CMAKE_SOURCE_DIR}/UI/Accessibility/Linux/LadybirdAtkObject.cpp
)
target_link_libraries(TestLadybirdAtkObject PRIVATE PkgConfig::ATK PkgConfig::GLIB)

ladybird_test(TestPrivateAccessibilityBus.cpp Accessibility/Linux LIBS LibCore)
target_sources(TestPrivateAccessibilityBus PRIVATE
    ${CMAKE_SOURCE_DIR}/UI/Accessibility/Linux/PrivateAccessibilityBus.cpp
)
target_link_libraries(TestPrivateAccessibilityBus PRIVATE PkgConfig::GLIB)

ladybird_test(TestAtkBridgeIntegration.cpp Accessibility/Linux LIBS LibWebView LibUnicode LibCore)
target_sources(TestAtkBridgeIntegration PRIVATE
    ${CMAKE_SOURCE_DIR}/UI/Accessibility/Linux/AtkBridge.cpp
    ${CMAKE_SOURCE_DIR}/UI/Accessibility/Linux/LadybirdAtkObject.cpp
    ${CMAKE_SOURCE_DIR}/UI/Accessibility/Linux/PrivateAccessibilityBus.cpp
)
target_link_libraries(TestAtkBridgeIntegration PRIVATE PkgConfig::ATK PkgConfig::GLIB ${CMAKE_DL_LIBS})

find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    add_test(
        NAME TestLadybirdOrcaScriptUtilities
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/Orca -p "test_*.py" -v
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Orca
    )
endif()

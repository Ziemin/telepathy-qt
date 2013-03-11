set(protocols_SRCS
    main.cpp
    cm-wrapper.cpp
    protocols.cpp)

set(protocols_MOC_SRCS
    cm-wrapper.h
    protocols.h)

tpqt_generate_mocs(${protocols_MOC_SRCS})

add_executable(protocols ${protocols_SRCS} ${protocols_MOC_SRCS})
target_link_libraries(protocols
    ${QT_QT5CORE_LIBRARY}
    ${QT_QT5DBUS_LIBRARY}
    ${QT_QT5GUI_LIBRARY}
    ${QT_QT5NETWORK_LIBRARY}
    ${QT_QT5WIDGETS_LIBRARY}
    ${QT_QT5XML_LIBRARY}
    telepathy-qt${QT_VERSION_MAJOR}
    ${TP_QT_EXECUTABLE_LINKER_FLAGS})

# install the DBus service and interface files in the correct place
include(ECMGenerateDBusServiceFile)
include(ECMConfiguredInstall)

# Generate and install dbus-activated systemd service
ecm_install_configured_files(INPUT app-org.sonicde.screenies.service.in DESTINATION ${KDE_INSTALL_SYSTEMDUSERUNITDIR})
# Upgrade the old unit name to a true systemd alias, not a second service that
# could start another process while waiting for the same bus name.
install(CODE "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink \"app-org.sonicde.screenies.service\" \"\$ENV{DESTDIR}${KDE_INSTALL_FULL_SYSTEMDUSERUNITDIR}/app-org.kde.spectacle.service\" COMMAND_ERROR_IS_FATAL ANY)")

foreach(service_name IN ITEMS org.sonicde.screenies org.kde.spectacle org.kde.Spectacle)
    ecm_generate_dbus_service_file(
        NAME ${service_name}
        EXECUTABLE "${KDE_INSTALL_FULL_BINDIR}/sonic-screenies --dbus"
        SYSTEMD_SERVICE app-org.sonicde.screenies.service
        DESTINATION ${KDE_INSTALL_DBUSSERVICEDIR}
    )
endforeach()

install(FILES org.sonicde.Screenies.xml org.kde.Spectacle.xml DESTINATION ${KDE_INSTALL_DBUSINTERFACEDIR})

project('clipboard-monitor', 'c')

gtk_dep = dependency('gtk+-3.0')
panel_dep = dependency('libxfce4panel-2.0')

shared_module('clipboard-monitor',
    'clipboard-monitor.c',
    dependencies: [gtk_dep, panel_dep],
    install: true,
    install_dir: get_option('libdir') / 'xfce4/panel/plugins',
    name_prefix: 'lib'
)

install_data('clipboard-monitor.desktop',
    install_dir: get_option('datadir') / 'xfce4/panel/plugins'
)

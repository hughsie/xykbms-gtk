project('xykbms-gtk', 'c',
  version : '0.1.0',
  license : 'LGPL-2.1-or-later',
  meson_version : '>=0.62.0',
  default_options : ['warning_level=2', 'c_std=c11'],
)

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  '-D_GNU_SOURCE',
]), language : 'c')

gio = dependency('gio-2.0', version : '>=2.76')
gtk = dependency('gtk4', version : '>=4.10')
zlib = dependency('zlib')

libxy = static_library('xy',
  sources : ['src/xy-config.c', 'src/xy-device.c'],
  dependencies : [gio, zlib],
)
libxy_dep = declare_dependency(link_with : libxy, dependencies : [gio])

executable('xykbms-tool',
  sources : ['src/xy-tool.c'],
  dependencies : [libxy_dep],
  install : true,
)

executable('xykbms-gtk',
  sources : ['src/xy-window.c'],
  dependencies : [libxy_dep, gtk],
  install : true,
)

install_data('data/70-xykbms.rules',
  install_dir : get_option('prefix') / 'lib' / 'udev' / 'rules.d',
)
install_data('data/io.github.hughsie.xykbms-gtk.desktop',
  install_dir : get_option('datadir') / 'applications',
)
install_data('data/io.github.hughsie.xykbms-gtk.metainfo.xml',
  install_dir : get_option('datadir') / 'metainfo',
)

appstreamcli = find_program('appstreamcli', required : false)
if appstreamcli.found()
  test('validate-metainfo', appstreamcli,
    args : ['validate', '--no-net', '--explain',
            files('data/io.github.hughsie.xykbms-gtk.metainfo.xml')],
  )
endif

test_exe = executable('xy-self-test',
  sources : ['src/xy-self-test.c'],
  dependencies : [libxy_dep],
)
test('xy-self-test', test_exe,
  env : ['XY_TEST_DATA=' + meson.project_source_root() / 'contrib'],
)

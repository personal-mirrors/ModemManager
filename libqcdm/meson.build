# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2021 Iñigo Martinez <inigomartinez@gmail.com>

project(
  'libqcdm', 'c',
  version: '0.1',
  license: 'GPL2',
  default_options: 'buildtype=debugoptimized',
  meson_version: '>= 0.37.0',
)

top_inc = include_directories('.')

# compiler flags
add_project_arguments('-DHAVE_CONFIG_H', language: 'c')

glib_dep = dependency('glib-2.0', version: '>= 2.18')

subdir('src')
subdir('tests')

configure_file(
  output: 'config.h',
  configuration: configuration_data(),
)

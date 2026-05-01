# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'mkmf'

append_cppflags('-std=c++17')
append_cflags('-fvisibility=hidden')

source_files = Dir.chdir(__dir__) { Dir.glob('**/*.cpp') }
source_directories = source_files.map { |path| File.dirname(path) }.uniq.sort

# mkmf exposes these globals as the extension-source configuration surface.
# rubocop:disable Style/GlobalVars
$VPATH.concat(
  source_directories
    .reject { |directory| directory == '.' }
    .map { |directory| "$(srcdir)/#{directory}" }
)

$srcs = source_files.map { |path| File.expand_path(path, __dir__) }
# rubocop:enable Style/GlobalVars

create_makefile('vajra/vajra')

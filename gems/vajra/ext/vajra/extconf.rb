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
source_basenames = source_files.map { |path| File.basename(path) }.sort
duplicate_basenames = source_basenames.tally.select { |_, count| count > 1 }.keys

raise "duplicate native source basenames are not supported: #{duplicate_basenames.join(', ')}" unless duplicate_basenames.empty?

# mkmf exposes these globals as the extension-source configuration surface.
# rubocop:disable Style/GlobalVars
$VPATH.concat(
  source_directories
    .reject { |directory| directory == '.' }
    .map { |directory| "$(srcdir)/#{directory}" }
)

$srcs = source_basenames
# rubocop:enable Style/GlobalVars

create_makefile('vajra/vajra')

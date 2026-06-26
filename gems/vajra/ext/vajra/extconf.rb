# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'mkmf'
require 'shellwords'

removed_c_compiler_flags = ['-Wsuggest-attribute=format']
configured_c_compiler = RbConfig::CONFIG.fetch('CC', '')
if configured_c_compiler.include?('gcc') && configured_c_compiler.scan('clang').empty?
  removed_c_compiler_flags.push(
    '-Wno-constant-logical-operand',
    '-Wno-parentheses-equality',
    '-Wno-self-assign'
  )
end

$CFLAGS = Shellwords.split($CFLAGS.to_s).reject { |flag| removed_c_compiler_flags.include?(flag) }.join(' ')
$warnflags = Shellwords.split($warnflags.to_s).reject { |flag| removed_c_compiler_flags.include?(flag) }.join(' ')

append_cflags('-fvisibility=hidden')
pkg_config('openssl') || (have_library('ssl') && have_library('crypto')) || raise('OpenSSL development files are required')

vendor_nghttp2_include = File.join(__dir__, 'vendor', 'nghttp2', 'lib', 'includes')
vendor_nghttp2_internal = File.join(__dir__, 'vendor', 'nghttp2', 'lib')
$INCFLAGS = "-I#{vendor_nghttp2_include} -I#{vendor_nghttp2_internal} #{$INCFLAGS}".strip
$defs.push(
  '-DNGHTTP2_STATICLIB',
  '-DBUILDING_NGHTTP2',
  '-DHAVE_ARPA_INET_H',
  '-DHAVE_NETINET_IN_H'
)

source_files = Dir.glob('**/*.{c,cpp}', base: __dir__)
source_directories = source_files.map { |path| File.dirname(path) }.uniq.sort
source_basenames = source_files.map { |path| File.basename(path) }.sort
duplicate_basenames = source_basenames.tally.select { |_, count| count > 1 }.keys

raise "duplicate native source basenames are not supported: #{duplicate_basenames.join(', ')}" unless duplicate_basenames.empty?

# mkmf exposes these globals as the extension-source configuration surface.
$CXXFLAGS = "#{$CXXFLAGS} -std=c++17".strip
$VPATH.concat(
  source_directories
    .reject { |directory| directory == '.' }
    .map { |directory| "$(srcdir)/#{directory}" }
)

$srcs = source_basenames

create_makefile('vajra/vajra')

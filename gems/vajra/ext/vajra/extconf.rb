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
    '-Wno-dll-attribute-on-redeclaration',
    '-Wno-parentheses-equality',
    '-Wno-self-assign'
  )
end

$CFLAGS = Shellwords.split($CFLAGS.to_s).reject { |flag| removed_c_compiler_flags.include?(flag) }.join(' ')
$CXXFLAGS = Shellwords.split($CXXFLAGS.to_s).reject { |flag| removed_c_compiler_flags.include?(flag) }.join(' ')
$warnflags = Shellwords.split($warnflags.to_s).reject { |flag| removed_c_compiler_flags.include?(flag) }.join(' ')

host_os = RbConfig::CONFIG.fetch('host_os', '')
windows_mingw = host_os.include?('mingw')
windows_cygwin = host_os.include?('cygwin')
windows_msvc = host_os.include?('mswin') || configured_c_compiler.match?(%r{(?:^|[\\/])cl(?:\.exe)?(?:\s|$)}i)
raise 'Vajra supports Windows only with RubyInstaller UCRT/MinGW Ruby' if windows_msvc || windows_cygwin

windows_host = windows_mingw
$defs.push('-DVAJRA_TEST_FAULT_INJECTION') if windows_host && ENV['VAJRA_TEST_FAULT_INJECTION'] == '1'

if windows_mingw
  mingw_warning_flags = %w[-Wall -Wextra -Wpedantic -Werror -Wno-cpp -Wno-unused-parameter]
  # Ruby 3.2.11's public RString initializer is intentionally partial and trips
  # newer MinGW GCC releases when extension warnings are promoted to errors.
  mingw_warning_flags << '-Wno-missing-field-initializers' if RUBY_VERSION.start_with?('3.2.')
  $CFLAGS = [$CFLAGS, *mingw_warning_flags].join(' ').strip
  $CXXFLAGS = [$CXXFLAGS, '-std=c++17', *mingw_warning_flags].join(' ').strip
else
  append_cflags('-fvisibility=hidden')
  $CXXFLAGS = "#{$CXXFLAGS} -std=c++17".strip
end

openssl_root = ENV.fetch('OPENSSL_ROOT_DIR', nil)
dir_config('openssl', File.join(openssl_root, 'include'), File.join(openssl_root, 'lib')) unless openssl_root.to_s.empty?
openssl_found = pkg_config('openssl') || (have_library('ssl') && have_library('crypto'))
raise('OpenSSL development files matching the Ruby compiler ABI are required') unless openssl_found

if windows_host
  have_library('ws2_32') || raise('Winsock development library is required')
  have_library('psapi') || raise('Windows process API development library is required')
end

vendor_nghttp2_include = File.join(__dir__, 'vendor', 'nghttp2', 'lib', 'includes')
vendor_nghttp2_internal = File.join(__dir__, 'vendor', 'nghttp2', 'lib')
$INCFLAGS = "-I#{vendor_nghttp2_include} -I#{vendor_nghttp2_internal} #{$INCFLAGS}".strip
$defs.push(
  '-DNGHTTP2_STATICLIB',
  '-DBUILDING_NGHTTP2'
)
$defs.push('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN') if windows_host
$defs.push('-DHAVE_ARPA_INET_H', '-DHAVE_NETINET_IN_H') unless windows_host

source_files = Dir.glob('**/*.{c,cpp}', base: __dir__)
if windows_host
  source_files.delete('runtime/native_runtime.cpp')
else
  source_files.delete('runtime/native_runtime_windows.cpp')
end
source_directories = source_files.map { |path| File.dirname(path) }.uniq.sort
source_basenames = source_files.map { |path| File.basename(path) }.sort
duplicate_basenames = source_basenames.tally.select { |_, count| count > 1 }.keys

raise "duplicate native source basenames are not supported: #{duplicate_basenames.join(', ')}" unless duplicate_basenames.empty?

# mkmf exposes these globals as the extension-source configuration surface.
$VPATH.concat(
  source_directories
    .reject { |directory| directory == '.' }
    .map { |directory| "$(srcdir)/#{directory}" }
)

$srcs = source_basenames

create_makefile('vajra/vajra')

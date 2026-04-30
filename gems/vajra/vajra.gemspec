# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
version = File.read(File.expand_path('lib/vajra/version.rb', __dir__))
              .match(/VERSION\s*=\s*['"]([^'"]+)['"]/)[1]

Gem::Specification.new do |spec|
  spec.name = 'vajra'
  spec.version = version
  spec.authors = ['Nitesh Purohit', 'Codevedas Inc.']
  spec.email = ['nitesh.purohit.it@gmail.com', 'team@codevedas.com']

  spec.summary = 'Native Ruby application server packaging and runtime bridge.'
  spec.description = <<~DESCRIPTION.strip
    Vajra is a native Ruby application server implemented in C++ as a Ruby
    extension. This package owns the Ruby entrypoints, executable, and native
    build contract for the Vajra runtime.
  DESCRIPTION
  spec.homepage = 'https://github.com/Code-Vedas/vajra'
  spec.license = 'MIT'
  spec.required_ruby_version = '>= 3.2.0'

  spec.metadata['bug_tracker_uri'] = "#{spec.homepage}/issues"
  spec.metadata['changelog_uri'] = "#{spec.homepage}/blob/main/CHANGELOG.md"
  spec.metadata['documentation_uri'] = "#{spec.homepage}/tree/main/docs"
  spec.metadata['funding_uri'] = 'https://github.com/sponsors/Code-Vedas'
  spec.metadata['homepage_uri'] = spec.homepage
  spec.metadata['source_code_uri'] = "#{spec.homepage}.git"
  spec.metadata['support_uri'] = "#{spec.homepage}/issues"
  spec.metadata['rubygems_mfa_required'] = 'true'
  spec.bindir = 'exe'
  spec.executables = ['vajra']
  spec.files = Dir.chdir(File.expand_path(__dir__)) do
    Dir[
      '{bin,exe,ext,lib,sig}/**/*',
      'LICENSE',
      'README.md',
      'Rakefile',
      '.rspec',
      '.reek.yml',
      '.rubocop.yml',
      '.ruby-version'
    ]
  end
  spec.require_paths = ['lib']
  spec.extensions = ['ext/vajra/extconf.rb']
end

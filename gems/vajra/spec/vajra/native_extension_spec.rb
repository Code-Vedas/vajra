# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'fileutils'
require 'tmpdir'

RSpec.describe Vajra::NativeExtension do
  let(:failing_loader) do
    proc { |_path| raise LoadError, 'cannot load such file -- vajra/vajra' }
  end

  it 'raises an actionable error when the native extension cannot be loaded' do
    expect { described_class.load!(loader: failing_loader) }.to raise_error(LoadError) do |error|
      expect(error.message).to include('bundle exec rake compile')
      expect(error.message).to include("Ruby ABI: #{RUBY_PLATFORM}")
      expect(error.message).to include('requires RubyInstaller UCRT Ruby')
    end
  end

  it 'preserves the original load error backtrace' do
    backtrace_loader = proc do |_path|
      raise LoadError, 'cannot load such file -- vajra/vajra'
    rescue LoadError => e
      raise e, e.message, ['/tmp/native_extension.rb:12']
    end

    expect { described_class.load!(loader: backtrace_loader) }.to raise_error(LoadError) do |e|
      expect(e.backtrace.first).to eq('/tmp/native_extension.rb:12')
    end
  end

  it 'loads only the native extension for the current platform' do
    loader = instance_double(Method, call: true)
    packaged_extension = File.join(
      described_class.packaged_native_root,
      "vajra.#{RbConfig::CONFIG.fetch('DLEXT')}"
    )
    expected_extension = if File.file?(packaged_extension)
                           packaged_extension
                         else
                           implementation_root = File.dirname(described_class.method(:load!).source_location.fetch(0))
                           File.expand_path("vajra/vajra.#{RbConfig::CONFIG.fetch('DLEXT')}", implementation_root)
                         end

    described_class.load!(loader: loader)

    expect(loader).to have_received(:call).with(expected_extension)
  end

  it 'loads the ABI-scoped extension from a precompiled package' do
    loader = instance_double(Method, call: true)

    Dir.mktmpdir('vajra-native-package') do |dir|
      extension = File.join(dir, "vajra.#{RbConfig::CONFIG.fetch('DLEXT')}")
      File.write(extension, '')
      allow(described_class).to receive(:packaged_native_root).and_return(dir)

      expect(described_class.load!(loader:)).to be(true)
      expect(loader).to have_received(:call).with(extension)
    end
  end

  it 'normalizes a callable loader result to a boolean' do
    loader = instance_double(Method, call: Object.new)

    expect(described_class.load!(loader: loader, extension_path: '/tmp/vajra.bundle')).to be(true)
  end

  it 'rejects packaged extensions for another Windows ABI' do
    Dir.mktmpdir('vajra-native-abi') do |dir|
      metadata_path = File.join(dir, 'native_abi.json')
      File.write(
        metadata_path,
        JSON.generate(
          platform: 'x64-mswin64',
          ruby_api_version: '0.0.0',
          architecture: 'x64-mswin64',
          compiler_family: 'msvc',
          runtime_abi: 'msvc'
        )
      )

      expect { described_class.validate_abi!(metadata_path:) }.to raise_error(
        LoadError,
        /Vajra native extension ABI mismatch/
      )
    end
  end

  it 'discovers and rejects foreign ABI metadata before selecting an extension' do
    Dir.mktmpdir('vajra-native-packages') do |dir|
      api_version = RbConfig::CONFIG.fetch('ruby_version')
      native_root = File.join(dir, 'x64-mswin64', api_version)
      FileUtils.mkdir_p(native_root)
      File.write(
        File.join(native_root, 'native_abi.json'),
        JSON.generate(
          platform: 'x64-mswin64',
          ruby_api_version: api_version,
          architecture: 'x64-mswin64',
          compiler_family: 'msvc',
          runtime_abi: 'msvc'
        )
      )
      allow(described_class).to receive(:native_packages_root).and_return(dir)

      expect { described_class.load!(loader: instance_double(Method)) }.to raise_error(
        LoadError,
        /Package ABI: x64-mswin64.*Runtime ABI:/m
      )
    end
  end

  it 'rejects packages containing multiple ABI metadata files for one Ruby API' do
    Dir.mktmpdir('vajra-native-packages') do |dir|
      api_version = RbConfig::CONFIG.fetch('ruby_version')
      %w[x64-mingw-ucrt x64-mswin64].each do |platform|
        native_root = File.join(dir, platform, api_version)
        FileUtils.mkdir_p(native_root)
        File.write(File.join(native_root, 'native_abi.json'), '{}')
      end
      allow(described_class).to receive(:native_packages_root).and_return(dir)

      expect { described_class.packaged_native_metadata_path }.to raise_error(
        LoadError,
        /multiple ABI metadata files/
      )
    end
  end

  it 'rejects a native package containing only a foreign Ruby API' do
    Dir.mktmpdir('vajra-native-packages') do |dir|
      foreign_api = '0.0.0'
      native_root = File.join(dir, 'x64-mingw-ucrt', foreign_api)
      FileUtils.mkdir_p(native_root)
      File.write(File.join(native_root, 'native_abi.json'), '{}')
      allow(described_class).to receive(:native_packages_root).and_return(dir)

      expect { described_class.packaged_native_metadata_path }.to raise_error(
        LoadError,
        /Package Ruby APIs: #{Regexp.escape(foreign_api)}.*Runtime Ruby API: /m
      )
    end
  end

  it 'reports every packaged API when none matches the running Ruby' do
    Dir.mktmpdir('vajra-native-packages') do |dir|
      %w[1.0.0 2.0.0].each do |api_version|
        native_root = File.join(dir, 'x64-mingw-ucrt', api_version)
        FileUtils.mkdir_p(native_root)
        File.write(File.join(native_root, 'native_abi.json'), '{}')
      end
      allow(described_class).to receive(:native_packages_root).and_return(dir)

      expect { described_class.packaged_native_metadata_path }.to raise_error(
        LoadError,
        /Package Ruby APIs: 1\.0\.0, 2\.0\.0/
      )
    end
  end

  it 'selects the current API from a valid merged native package' do
    Dir.mktmpdir('vajra-native-packages') do |dir|
      current_api = RbConfig::CONFIG.fetch('ruby_version')
      %W[0.0.0 #{current_api}].each do |api_version|
        native_root = File.join(dir, 'x64-mingw-ucrt', api_version)
        FileUtils.mkdir_p(native_root)
        File.write(File.join(native_root, 'native_abi.json'), '{}')
      end
      allow(described_class).to receive(:native_packages_root).and_return(dir)

      expect(described_class.packaged_native_metadata_path).to eq(
        File.join(dir, 'x64-mingw-ucrt', current_api, 'native_abi.json')
      )
    end
  end

  it 'allows source gems without native ABI metadata' do
    Dir.mktmpdir('vajra-native-packages') do |dir|
      allow(described_class).to receive(:native_packages_root).and_return(dir)

      expect(described_class.validate_abi!).to be(true)
    end
  end

  it 'selects the packaged native root from discovered metadata' do
    Dir.mktmpdir('vajra-native-packages') do |dir|
      native_root = File.join(dir, 'x64-mingw-ucrt', RbConfig::CONFIG.fetch('ruby_version'))
      FileUtils.mkdir_p(native_root)
      File.write(File.join(native_root, 'native_abi.json'), '{}')
      allow(described_class).to receive(:native_packages_root).and_return(dir)

      expect(described_class.packaged_native_root).to eq(native_root)
    end
  end

  it 'rejects an explicitly selected package whose ABI metadata is missing' do
    Dir.mktmpdir('vajra-native-packages') do |dir|
      missing_metadata = File.join(dir, 'native_abi.json')

      expect { described_class.validate_abi!(metadata_path: missing_metadata) }.to raise_error(
        LoadError,
        /ABI metadata is missing/
      )
    end
  end

  it 'accepts metadata matching the current Windows Ruby ABI contract' do
    stub_const('RUBY_PLATFORM', 'x64-mingw-ucrt')
    allow(Gem).to receive(:win_platform?).and_return(true)

    Dir.mktmpdir('vajra-native-abi') do |dir|
      metadata_path = File.join(dir, 'native_abi.json')
      File.write(
        metadata_path,
        JSON.generate(
          platform: 'x64-mingw-ucrt',
          ruby_api_version: RbConfig::CONFIG.fetch('ruby_version'),
          architecture: RbConfig::CONFIG.fetch('arch'),
          compiler_family: 'mingw',
          runtime_abi: 'ucrt'
        )
      )

      expect(described_class.validate_abi!(metadata_path:)).to be(true)
    end
  end

  it 'rejects MSVC-built Ruby with an actionable installation message' do
    stub_const('RUBY_PLATFORM', 'x64-mswin64')
    allow(Gem).to receive(:win_platform?).and_return(true)

    expect { described_class.ensure_supported_windows_abi! }.to raise_error(
      LoadError,
      /does not support MSVC-built Ruby.*x64-mingw-ucrt/m
    )
  end

  it 'accepts RubyInstaller UCRT Ruby on Windows' do
    stub_const('RUBY_PLATFORM', 'x64-mingw-ucrt')
    allow(Gem).to receive(:win_platform?).and_return(true)

    expect(described_class.ensure_supported_windows_abi!).to be(true)
  end

  it 'does not apply the Windows ABI restriction on other platforms' do
    allow(Gem).to receive(:win_platform?).and_return(false)

    expect(described_class.ensure_supported_windows_abi!).to be(true)
  end

  it 'rejects malformed native ABI metadata' do
    Dir.mktmpdir('vajra-native-abi') do |dir|
      metadata_path = File.join(dir, 'native_abi.json')
      File.write(metadata_path, '{')

      expect { described_class.validate_abi!(metadata_path:) }.to raise_error(
        LoadError,
        /Invalid Vajra native ABI metadata/
      )
    end
  end
end

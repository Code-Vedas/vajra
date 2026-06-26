# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

RSpec.describe Vajra::Rails do
  before do
    described_class.uninstall!
    hide_const('Rails') if Object.const_defined?(:Rails)
  end

  after do
    described_class.uninstall!
  end

  def build_application(initialized: false, initialize_error: nil)
    Class.new do
      attr_reader :initialize_calls

      def initialize(initialized, initialize_error)
        @initialized = initialized
        @initialize_error = initialize_error
        @initialize_calls = 0
      end

      def call(_env)
        [200, { 'Content-Type' => 'text/plain' }, ['OK']]
      end

      def initialized?
        @initialized
      end

      def initialize!
        @initialize_calls += 1
        raise @initialize_error if @initialize_error

        @initialized = true
        self
      end
    end.new(initialized, initialize_error)
  end

  it 'installs and initializes the configured Rails application' do
    application = build_application
    stub_const('Rails', Module.new)
    Rails.singleton_class.send(:define_method, :application) { application }

    expect(described_class.install!).to equal(application)
    expect(application.initialized?).to be(true)
    expect(application.initialize_calls).to eq(1)
    expect(Vajra::Internal::RackExecution.installed?).to be(true)
  end

  it 'does not reinitialize an already initialized Rails application' do
    application = build_application(initialized: true)
    stub_const('Rails', Module.new)
    Rails.singleton_class.send(:define_method, :application) { application }

    expect(described_class.install!).to equal(application)
    expect(application.initialize_calls).to eq(0)
  end

  it 'initializes uninitialized Rails applications inside the executor when available' do
    executor = Class.new do
      attr_reader :wrap_calls

      def initialize
        @wrap_calls = 0
      end

      def wrap
        @wrap_calls += 1
        yield
      end
    end.new
    application = build_application
    application.define_singleton_method(:executor) { executor }
    stub_const('Rails', Module.new)
    Rails.singleton_class.send(:define_method, :application) { application }

    expect(described_class.install!).to equal(application)

    expect(application.initialized?).to be(true)
    expect(application.initialize_calls).to eq(1)
    expect(executor.wrap_calls).to eq(1)
  end

  it 'uses the current Rails application executor when the explicit application matches' do
    executor = Class.new do
      attr_reader :wrap_calls

      def initialize
        @wrap_calls = 0
      end

      def wrap
        @wrap_calls += 1
        yield
      end
    end.new
    application = build_application
    stub_const('Rails', Module.new)
    Rails.singleton_class.send(:define_method, :application) { application }
    method_executor_calls = 0
    application.singleton_class.send(:define_method, :method) do |name|
      if name == :executor
        method_executor_calls += 1
        raise NameError, name.to_s if method_executor_calls == 1
      end

      super(name)
    end
    application.singleton_class.send(:define_method, :executor) { executor }

    expect(described_class.install!(application)).to equal(application)

    expect(application.initialized?).to be(true)
    expect(application.initialize_calls).to eq(1)
    expect(executor.wrap_calls).to eq(1)
  end

  it 'accepts an explicit Rails application argument' do
    application = build_application

    expect(described_class.install!(application)).to equal(application)
    expect(Vajra::Internal::RackExecution.installed?).to be(true)
  end

  it 'tracks whether the Rails adapter is installed' do
    application = build_application

    expect(described_class.installed?).to be(false)
    described_class.install!(application)
    expect(described_class.installed?).to be(true)
    described_class.uninstall!
    expect(described_class.installed?).to be(false)
  end

  it 'fails when no Rails application is configured' do
    expect { described_class.install! }
      .to raise_error(Vajra::Rails::Error, 'Rails application is not configured')
  end

  it 'fails when the configured Rails constant does not expose an application' do
    stub_const('Rails', Module.new)

    expect { described_class.install! }
      .to raise_error(Vajra::Rails::Error, 'Rails application is not configured')
  end

  it 'fails when the Rails application does not respond to call' do
    invalid_application = Object.new

    expect { described_class.install!(invalid_application) }
      .to raise_error(Vajra::Rails::Error, 'Rails application must respond to #call')
  end

  it 'fails when the Rails application does not respond to initialized?' do
    invalid_application = Object.new
    def invalid_application.call(_env)
      [200, {}, []]
    end

    expect { described_class.install!(invalid_application) }
      .to raise_error(Vajra::Rails::Error, 'Rails application must respond to #initialized?')
  end

  it 'fails when the Rails application does not respond to initialize!' do
    invalid_application = Object.new
    def invalid_application.call(_env)
      [200, {}, []]
    end

    def invalid_application.initialized?
      false
    end

    expect { described_class.install!(invalid_application) }
      .to raise_error(Vajra::Rails::Error, 'Rails application must respond to #initialize!')
  end

  it 'fails when the Rails application boot raises' do
    application = build_application(initialize_error: RuntimeError.new('boot exploded'))
    stub_const('Rails', Module.new)
    Rails.singleton_class.send(:define_method, :application) { application }

    expect { described_class.install! }
      .to raise_error(Vajra::Rails::Error, 'Rails application boot failed: RuntimeError: boot exploded')
  end

  it 'passes through adapter validation errors without rewrapping them' do
    expect { described_class.install!(Object.new) }
      .to raise_error(Vajra::Rails::Error, 'Rails application must respond to #call')
  end
end

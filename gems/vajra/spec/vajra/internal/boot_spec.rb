# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

RSpec.describe Vajra::Internal::Boot do
  after do
    described_class.install!
  end

  it 'tracks whether a boot coordinator is installed' do
    expect(described_class.installed?).to be(true)

    described_class.uninstall!

    expect(described_class.installed?).to be(false)
  end

  it 'accepts callable boot coordinator objects' do
    coordinator = Class.new do
      def call(_boot_request)
        { status: 'ready', role: 'custom_bootstrap' }
      end
    end.new

    expect(described_class.install!(coordinator)).to equal(coordinator)
  end

  it 'returns the default method coordinator when install! is called without arguments' do
    coordinator = described_class.install!

    expect(coordinator).to be_a(Method)
    expect(coordinator.name).to eq(:default_coordinator)
  end

  it 'rejects non-callable boot coordinators' do
    expect { described_class.install!(Object.new) }
      .to raise_error(TypeError, 'boot coordinator must respond to #call')
  end

  it 'reraises unrelated NameError values from coordinator validation' do
    coordinator = Class.new do
      def method(_name)
        raise NameError.new('unexpected missing method', :unexpected_method)
      end
    end.new

    expect { described_class.install!(coordinator) }
      .to raise_error(NameError, /unexpected missing method/)
  end

  it 'does not rewrite unrelated NameError failures as coordinator type errors' do
    allow(described_class).to receive(:__native_set_boot_callback__).and_raise(
      NoMethodError,
      "undefined method `__native_set_boot_callback__'"
    )

    expect { described_class.install!(->(_boot_request) { 'ready' }) }
      .to raise_error(NoMethodError, /__native_set_boot_callback__/)

    allow(described_class).to receive(:__native_set_boot_callback__).and_call_original
  end

  it 'does not mark the boot coordinator installed when native callback registration fails' do
    previous_coordinator = described_class.send(:configured_coordinator)

    allow(described_class).to receive(:__native_set_boot_callback__).and_raise(
      RuntimeError,
      'native callback registration failed'
    )

    expect { described_class.install!(->(_boot_request) { 'ready' }) }
      .to raise_error(RuntimeError, /native callback registration failed/)

    expect(described_class.send(:configured_coordinator)).to equal(previous_coordinator)
    allow(described_class).to receive(:__native_set_boot_callback__).and_call_original
  end

  it 'normalizes a successful boot result and boot request' do
    captured_request = nil
    described_class.install!(lambda { |boot_request|
      captured_request = boot_request
      { status: :ready, role: 'single_process_bootstrap' }
    })

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('ready')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to be_nil
    expect(captured_request).to eq(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )
  end

  it 'accepts string-keyed boot requests without symbolizing arbitrary keys' do
    captured_request = nil
    described_class.install!(lambda { |boot_request|
      captured_request = boot_request
      { status: :ready, role: 'single_process_bootstrap' }
    })

    status, role, diagnostic = described_class.call(
      'port' => 3000,
      'max_request_head_bytes' => 16_384,
      'runtime_role' => 'single_process_bootstrap',
      'unexpected_user_key' => 'ignored'
    )

    expect(status).to eq('ready')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to be_nil
    expect(captured_request).to include(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )
    expect(captured_request).not_to have_key(:unexpected_user_key)
  end

  it 'returns a structured failed result when the coordinator raises' do
    described_class.install!(->(_boot_request) { raise 'boot exploded' })

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['boot_callback_error', 'boot', 'RuntimeError: boot exploded']
    )
  end

  it 'uses the default coordinator when install! is called without arguments' do
    described_class.install!

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'ignored'
    )

    expect(status).to eq('ready')
    expect(role).to eq('ignored')
    expect(diagnostic).to be_nil
  end

  it 'accepts bare status values from the coordinator' do
    described_class.install!(->(_boot_request) { :pending })

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'ruby_worker_bootstrap'
    )

    expect(status).to eq('pending')
    expect(role).to eq('ruby_worker_bootstrap')
    expect(diagnostic).to be_nil
  end

  it 'accepts array boot results with array diagnostics' do
    described_class.install!(lambda do |_boot_request|
      ['failed', 'custom_bootstrap', ['boot_failed', 'boot', 'failed on purpose']]
    end)

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('custom_bootstrap')
    expect(diagnostic).to eq(['boot_failed', 'boot', 'failed on purpose'])
  end

  it 'accepts hash diagnostics and stringifies their fields' do
    described_class.install!(lambda do |_boot_request|
      {
        status: 'failed',
        role: 'custom_bootstrap',
        diagnostic: {
          code: :boot_failed,
          category: :boot,
          message: 123
        }
      }
    end)

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('custom_bootstrap')
    expect(diagnostic).to eq(%w[boot_failed boot 123])
  end

  it 'returns a contract failure when the coordinator returns an invalid result' do
    described_class.install!(->(_boot_request) { [:unknown, 'single_process_bootstrap', nil] })

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: unsupported boot status: unknown']
    )
  end

  it 'returns a contract failure when the coordinator returns an unsupported result type' do
    described_class.install!(->(_boot_request) { 123 })

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: boot coordinator must return a boot result']
    )
  end

  it 'returns a contract failure when the coordinator omits boot result fields' do
    described_class.install!(->(_boot_request) { { status: 'ready' } })

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: missing boot result field: role']
    )
  end

  it 'returns a contract failure when a failed boot result omits diagnostics' do
    described_class.install!(->(_boot_request) { { status: 'failed', role: 'custom_bootstrap' } })

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: failed boot results must include diagnostic details']
    )
  end

  it 'returns a contract failure when the coordinator returns an empty role' do
    described_class.install!(->(_boot_request) { { status: 'ready', role: '' } })

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: boot role must not be empty']
    )
  end

  it 'returns a contract failure when boot status coercion raises' do
    bad_status = Object.new
    def bad_status.to_s
      raise TypeError, 'status boom'
    end

    described_class.install!(->(_boot_request) { [bad_status, 'custom_bootstrap', nil] })

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: boot status must be coercible to String: status boom']
    )
  end

  it 'returns a contract failure when boot role coercion raises' do
    bad_role = Object.new
    def bad_role.to_s
      raise TypeError, 'role boom'
    end

    described_class.install!(->(_boot_request) { ['ready', bad_role, nil] })

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: boot role must be coercible to String: role boom']
    )
  end

  it 'returns a contract failure when the coordinator returns an invalid diagnostic shape' do
    described_class.install!(lambda do |_boot_request|
      { status: 'failed', role: 'custom_bootstrap', diagnostic: 'bad diagnostic' }
    end)

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: boot diagnostic must be a Hash or 3-item Array']
    )
  end

  it 'returns a contract failure when diagnostic value coercion raises' do
    bad_value = Object.new
    def bad_value.to_s
      raise TypeError, 'diagnostic boom'
    end

    described_class.install!(lambda do |_boot_request|
      { status: 'failed', role: 'custom_bootstrap', diagnostic: [:boot_failed, :boot, bad_value] }
    end)

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: boot diagnostic value must be coercible to String: diagnostic boom']
    )
  end

  it 'returns a contract failure when the coordinator returns a short diagnostic array' do
    described_class.install!(lambda do |_boot_request|
      { status: 'failed', role: 'custom_bootstrap', diagnostic: %w[boot_failed boot] }
    end)

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: boot diagnostic must be a Hash or 3-item Array']
    )
  end

  it 'returns a contract failure when diagnostic fields are missing' do
    described_class.install!(lambda do |_boot_request|
      { status: 'failed', role: 'custom_bootstrap', diagnostic: { code: 'boot_failed' } }
    end)

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: 'single_process_bootstrap'
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: missing boot diagnostic field: category']
    )
  end

  it 'returns a contract failure for malformed boot requests' do
    status, role, diagnostic = described_class.call([])

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: boot request must be a Hash']
    )
  end

  it 'returns a contract failure for boot requests with missing or invalid fields' do
    missing_status, missing_role, missing_diagnostic = described_class.call(
      port: 3000,
      runtime_role: 'single_process_bootstrap'
    )
    invalid_status, invalid_role, invalid_diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 'invalid',
      runtime_role: 'single_process_bootstrap'
    )

    expect(missing_status).to eq('failed')
    expect(missing_role).to eq('single_process_bootstrap')
    expect(missing_diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: missing boot request field: max_request_head_bytes']
    )

    expect(invalid_status).to eq('failed')
    expect(invalid_role).to eq('single_process_bootstrap')
    expect(invalid_diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: invalid value for Integer(): "invalid"']
    )
  end

  it 'returns a contract failure for boot requests with an empty runtime role' do
    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: ''
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: runtime role must not be empty']
    )
  end

  it 'returns a contract failure for boot requests when runtime role coercion raises' do
    bad_runtime_role = Object.new
    def bad_runtime_role.to_s
      raise TypeError, 'runtime role boom'
    end

    status, role, diagnostic = described_class.call(
      port: 3000,
      max_request_head_bytes: 16_384,
      runtime_role: bad_runtime_role
    )

    expect(status).to eq('failed')
    expect(role).to eq('single_process_bootstrap')
    expect(diagnostic).to eq(
      ['invalid_boot_contract', 'contract', 'Vajra::Internal::Boot::BootContractError: runtime role must be coercible to String: runtime role boom']
    )
  end
end

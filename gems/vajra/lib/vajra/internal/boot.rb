# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

module Vajra
  module Internal
    # Coordinates Ruby boot readiness for the native startup path.
    module Boot
      module_function

      STATUSES = {
        ready: 'ready',
        failed: 'failed',
        pending: 'pending'
      }.freeze
      SINGLE_PROCESS_BOOTSTRAP_ROLE = 'single_process_bootstrap'

      # Raised when the Ruby/native boot callback contract is malformed.
      class BootContractError < StandardError
      end
      # Stores the currently configured boot coordinator callback.
      BootState = Struct.new(:coordinator)

      BOOT_MUTEX = Mutex.new
      BOOT_STATE = BootState.new(nil)

      def install!(coordinator = method(:default_coordinator))
        validate_coordinator!(coordinator)

        BOOT_MUTEX.synchronize { BOOT_STATE.coordinator = coordinator }
        __native_set_boot_callback__(bootstrap_callback)
        coordinator
      end

      def uninstall!
        BOOT_MUTEX.synchronize { BOOT_STATE.coordinator = nil }
        __native_set_boot_callback__(nil)
      end

      def installed?
        !configured_coordinator.equal?(nil)
      end

      def call(boot_request)
        normalize_boot_result(active_coordinator.call(normalize_boot_request(boot_request)))
      rescue StandardError => e
        failure_result(
          code: normalized_failure_code(e),
          category: failure_category_for(e),
          message: "#{e.class}: #{e.message}"
        )
      end

      def default_coordinator(_boot_request)
        {
          status: STATUSES.fetch(:ready),
          role: SINGLE_PROCESS_BOOTSTRAP_ROLE
        }
      end
      private_class_method :default_coordinator

      def bootstrap_callback
        proc { |boot_request| Vajra::Internal::Boot.call(boot_request) }
      end
      private_class_method :bootstrap_callback

      def validate_coordinator!(coordinator)
        coordinator.method(:call)
      rescue NameError => e
        raise unless e.name == :call

        raise TypeError, 'boot coordinator must respond to #call'
      end
      private_class_method :validate_coordinator!

      def configured_coordinator
        BOOT_MUTEX.synchronize { BOOT_STATE.coordinator }
      end
      private_class_method :configured_coordinator

      def active_coordinator
        configured_coordinator || method(:default_coordinator)
      end
      private_class_method :active_coordinator

      def normalize_boot_request(boot_request)
        raise BootContractError, 'boot request must be a Hash' unless boot_request.is_a?(Hash)

        {
          port: Integer(fetch_boot_request_value(boot_request, :port)),
          max_request_head_bytes: Integer(fetch_boot_request_value(boot_request, :max_request_head_bytes)),
          runtime_role: String(fetch_boot_request_value(boot_request, :runtime_role))
        }
      rescue KeyError => e
        raise BootContractError, "missing boot request field: #{e.key}"
      rescue ArgumentError, TypeError => e
        raise BootContractError, e.message
      end
      private_class_method :normalize_boot_request

      def fetch_boot_request_value(boot_request, key)
        return boot_request.fetch(key) if boot_request.key?(key)

        boot_request.fetch(String(key))
      end
      private_class_method :fetch_boot_request_value

      def normalize_boot_result(result)
        status, role, diagnostic = unpack_result(result)
        normalized_status = normalize_status(status)
        normalized_role = coerce_contract_string(role, 'boot role')
        raise BootContractError, 'boot role must not be empty' if normalized_role.empty?

        normalized_diagnostic = normalize_diagnostic(diagnostic)
        if [normalized_status, normalized_diagnostic] == [STATUSES.fetch(:failed), nil]
          raise BootContractError, 'failed boot results must include diagnostic details'
        end

        [normalized_status, normalized_role, normalized_diagnostic]
      end
      private_class_method :normalize_boot_result

      def unpack_result(result)
        return [result.fetch(:status), result.fetch(:role), result[:diagnostic]] if result.is_a?(Hash)
        return [result, SINGLE_PROCESS_BOOTSTRAP_ROLE, nil] if result.is_a?(String) || result.is_a?(Symbol)

        return result if result.is_a?(Array) && result.length == 3

        raise BootContractError, 'boot coordinator must return a boot result'
      rescue KeyError => e
        raise BootContractError, "missing boot result field: #{e.key}"
      end
      private_class_method :unpack_result

      def normalize_status(status)
        normalized_status = coerce_contract_string(status, 'boot status')
        return normalized_status if STATUSES.value?(normalized_status)

        raise BootContractError, "unsupported boot status: #{normalized_status}"
      end
      private_class_method :normalize_status

      def normalize_diagnostic(diagnostic)
        diagnostic&.then { |value| stringify_diagnostic_values(diagnostic_values_from(value)) }
      rescue KeyError => e
        raise BootContractError, "missing boot diagnostic field: #{e.key}"
      end
      private_class_method :normalize_diagnostic

      def diagnostic_values_from(diagnostic)
        case diagnostic
        when Hash
          [
            diagnostic.fetch(:code),
            diagnostic.fetch(:category),
            diagnostic.fetch(:message)
          ]
        when Array
          invalid_boot_diagnostic! unless diagnostic.length == 3

          diagnostic
        else
          invalid_boot_diagnostic!
        end
      end
      private_class_method :diagnostic_values_from

      def stringify_diagnostic_values(diagnostic_values)
        diagnostic_values.map { |value| coerce_contract_string(value, 'boot diagnostic value') }
      end
      private_class_method :stringify_diagnostic_values

      def coerce_contract_string(value, field_name)
        String(value)
      rescue TypeError, ArgumentError => e
        raise BootContractError, "#{field_name} must be coercible to String: #{e.message}"
      end
      private_class_method :coerce_contract_string

      def invalid_boot_diagnostic!
        raise BootContractError, 'boot diagnostic must be a Hash or 3-item Array'
      end
      private_class_method :invalid_boot_diagnostic!

      def failure_result(code:, category:, message:)
        [
          STATUSES.fetch(:failed),
          SINGLE_PROCESS_BOOTSTRAP_ROLE,
          [String(code), String(category), String(message)]
        ]
      end
      private_class_method :failure_result

      def normalized_failure_code(error)
        return 'invalid_boot_contract' if error.is_a?(BootContractError)

        'boot_callback_error'
      end
      private_class_method :normalized_failure_code

      def failure_category_for(error)
        return 'contract' if error.is_a?(BootContractError)

        'boot'
      end
      private_class_method :failure_category_for
    end
  end
end

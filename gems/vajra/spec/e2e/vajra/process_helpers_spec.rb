# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'support'

RSpec.describe 'Vajra e2e process helpers', :e2e, :integration do # rubocop:disable RSpec/DescribeClass
  let(:helper_host) do
    Class.new do
      include VajraE2ESupport
    end.new
  end

  it 'returns captured output when read_nonblock raises IOError' do
    output = instance_double(IO)
    first_chunk = true

    allow(output).to receive(:read_nonblock) do
      if first_chunk
        first_chunk = false
        'partial output'
      else
        raise IOError, 'closed stream'
      end
    end

    expect(helper_host.read_available_output(output)).to eq('partial output')
  end
end

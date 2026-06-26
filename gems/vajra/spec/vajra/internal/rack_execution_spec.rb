# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

RSpec.describe Vajra::Internal::RackExecution do
  after do
    described_class.uninstall!
  end

  it 'tracks whether a Rack app is installed' do
    expect(described_class.installed?).to be(false)

    described_class.install!(->(_env) { [200, { 'Content-Type' => 'text/plain' }, ['OK']] })

    expect(described_class.installed?).to be(true)
  end

  it 'accepts callable rack app objects' do
    app = Class.new do
      def call(_env)
        [200, { 'Content-Type' => 'text/plain' }, ['OK']]
      end
    end.new

    expect(described_class.install!(app)).to equal(app)
  end

  it 'rejects non-callable rack app objects' do
    expect { described_class.install!(Object.new) }
      .to raise_error(TypeError, 'Rack app must respond to #call')
  end

  it 'builds the Rack env and normalizes the response body and headers' do
    captured_env = nil
    captured_input_reads = nil
    request_body = "abc\x00".b
    described_class.configure_threads!(1)
    headers = Class.new do
      def each
        yield :content_type, 'text/plain'
      end
    end.new
    closing_body = Class.new do
      def initialize(chunks)
        @chunks = chunks
      end

      def each(&block)
        @chunks.each(&block)
      end

      def close
        @closed = true
      end

      def closed?
        @closed == true
      end
    end.new(%w[O K])

    described_class.install!(lambda { |env|
      captured_env = env
      input = env.fetch('rack.input')
      captured_input_reads = [input.read(2), input.read]
      input.rewind
      captured_input_reads << input.read
      [200, headers, closing_body]
    })

    status, headers, body = described_class.call(
      [
        ['REQUEST_METHOD', 'GET'],
        ['SCRIPT_NAME', ''],
        ['PATH_INFO', '/projects'],
        ['QUERY_STRING', 'filter=active'],
        ['rack.url_scheme', 'http']
      ],
      request_body
    )

    expect(status).to eq(200)
    expect(headers).to eq([['content-type', 'text/plain']])
    expect(body).to eq(%w[O K])
    expect(closing_body.closed?).to be(true)
    expect(captured_env).to include(
      'REQUEST_METHOD' => 'GET',
      'SCRIPT_NAME' => '',
      'PATH_INFO' => '/projects',
      'QUERY_STRING' => 'filter=active',
      'rack.version' => [1, 6],
      'rack.url_scheme' => 'http',
      'rack.multithread' => false,
      'rack.multiprocess' => false,
      'rack.run_once' => false
    )
    expect(captured_env.fetch('rack.input')).to be_a(Vajra::NativeInput)
    expect(captured_input_reads).to eq(['ab', "c\x00".b, "abc\x00".b])
    expect { captured_env.fetch('rack.input').read }.to raise_error(IOError, 'rack.input is closed')
    expect(captured_env.fetch('rack.errors')).to equal($stderr)
  end

  it 'returns nil when no Rack app is installed' do
    expect(described_class.call([%w[REQUEST_METHOD GET]], ''.b)).to be_nil
  end

  it 'wraps native-built Rack env execution in request tracing' do
    env = { 'REQUEST_METHOD' => 'GET', 'rack.input' => Vajra::NativeInput.from_string('') }
    app = lambda { |rack_env|
      expect(rack_env).to equal(env)
      [204, {}, []]
    }
    allow(Vajra::Internal::Tracing).to receive(:with_request_span).and_wrap_original do |original, rack_env, &block|
      expect(rack_env).to equal(env)
      original.call(rack_env, &block)
    end

    expect(described_class.call_native(app, env)).to eq([204, {}, []])
    expect(Vajra::Internal::Tracing).to have_received(:with_request_span).with(env)
  ensure
    env&.fetch('rack.input')&.close
  end

  it 'collects a body that does not implement close' do
    described_class.install!(->(_env) { [204, {}, []] })

    status, headers, body = described_class.call([%w[REQUEST_METHOD HEAD]], ''.b)

    expect(status).to eq(204)
    expect(headers).to eq([])
    expect(body).to eq([])
  end

  it 'preserves binary body bytes' do
    binary_chunk = Class.new do
      def to_s
        "\xFF\0\x80".b
      end
    end.new
    described_class.install!(->(_env) { [200, {}, [binary_chunk]] })

    _status, _headers, body = described_class.call([%w[REQUEST_METHOD GET]], ''.b)

    expect(body.fetch(0).encoding).to eq(Encoding::BINARY)
    expect(body.fetch(0).bytes).to eq([255, 0, 128])
  end

  it 'reports rack.multithread from the configured maximum thread count' do
    captured_envs = []
    described_class.install!(lambda { |env|
      captured_envs << env
      [204, {}, []]
    })

    described_class.configure_threads!(1)
    described_class.call([%w[REQUEST_METHOD GET]], ''.b)
    described_class.configure_threads!(2)
    described_class.call([%w[REQUEST_METHOD GET]], ''.b)

    expect(captured_envs.map { |env| env.fetch('rack.multithread') }).to eq([false, true])
  end

  it 'coerces mutable non-binary request bodies into binary native rack.input' do
    captured_env = nil
    captured_body = nil
    request_body = +"snowman-\u2603"

    described_class.install!(lambda { |env|
      captured_env = env
      captured_body = env.fetch('rack.input').read
      [204, {}, []]
    })

    described_class.call([%w[REQUEST_METHOD POST]], request_body)

    input = captured_env.fetch('rack.input')
    expect(input).to be_a(Vajra::NativeInput)
    expect(input.external_encoding).to eq(Encoding::BINARY)
    expect(captured_body).to eq(request_body.b)
    expect { input.read }.to raise_error(IOError, 'rack.input is closed')
    expect(request_body.encoding).to eq(Encoding::BINARY)
    expect(described_class.send(:binary_request_body, request_body)).to equal(request_body)
  end

  it 'copies frozen non-binary request bodies before binary native rack.input coercion' do
    captured_env = nil
    captured_body = nil
    request_body = "snowman-\u2603"

    described_class.install!(lambda { |env|
      captured_env = env
      captured_body = env.fetch('rack.input').read
      [204, {}, []]
    })

    described_class.call([%w[REQUEST_METHOD POST]], request_body)

    input = captured_env.fetch('rack.input')
    expect(input).to be_a(Vajra::NativeInput)
    expect(input.external_encoding).to eq(Encoding::BINARY)
    expect(captured_body).to eq(request_body.b)
    expect { input.read }.to raise_error(IOError, 'rack.input is closed')
    expect(request_body.encoding).not_to eq(Encoding::BINARY)
  end

  it 'rejects non-string request bodies' do
    described_class.install!(->(_env) { [204, {}, []] })

    expect { described_class.call([%w[REQUEST_METHOD POST]], nil) }
      .to raise_error(TypeError, 'request_body must be a String')
  end

  it 'accepts native Rack input objects without replacing them' do
    captured_env = nil
    captured_body = nil
    input = Vajra::NativeInput.from_string('abc')
    described_class.install!(lambda { |env|
      captured_env = env
      captured_body = env.fetch('rack.input').read
      [204, {}, []]
    })

    described_class.call([%w[REQUEST_METHOD POST]], input)

    expect(captured_env.fetch('rack.input')).to equal(input)
    expect(captured_body).to eq('abc')
    expect { input.read }.to raise_error(IOError, 'rack.input is closed')
  end

  it 'rejects invalid Rack input replacements' do
    described_class.install!(->(_env) { [204, {}, []] })

    expect { described_class.call([%w[REQUEST_METHOD POST]], Object.new) }
      .to raise_error(TypeError, 'rack.input must respond to #read')
  end

  it 'supports native request input reads, lines, rewind, and outbuf replacement' do
    input = Vajra::NativeInput.from_string("ab\ncdef\nghi")

    expect(input.external_encoding).to eq(Encoding::BINARY)
    expect(input.read(0)).to eq('')
    expect(input.read(2)).to eq('ab')
    expect(input.gets).to eq("\n")

    outbuf = +'stale'
    expect(input.read(3, outbuf)).to eq('cde')
    expect(outbuf).to eq('cde')

    expect(input.gets).to eq("f\n")
    expect(input.gets(nil)).to eq('ghi')
    expect(input.gets).to be_nil
    expect(input.read(1)).to be_nil

    input.rewind
    expect(input.each.to_a).to eq(%W[ab\n cdef\n ghi])
    expect(input.each).to be_a(Enumerator)
  ensure
    input&.close
  end

  it 'spills native request input to a tempfile and supports full-body reads' do
    input = Vajra::NativeInput.from_string('abcdef')

    expect(input.read).to eq('abcdef')
    input.rewind
    expect(input.read).to eq('abcdef')
  ensure
    input&.close
  end

  it 'supports chunked reads and rewind for large native request input' do
    captured_reads = nil
    body = ('a' * (1024 * 1024)) + ('b' * 64)

    described_class.install!(lambda { |env|
      input = env.fetch('rack.input')
      captured_reads = [input.read(8192).bytesize, input.read.bytesize]
      input.rewind
      captured_reads << input.read.bytesize
      [204, {}, []]
    })

    described_class.call([%w[REQUEST_METHOD POST]], body)

    expect(captured_reads).to eq([8192, body.bytesize - 8192, body.bytesize])
  end

  it 'raises from native request input reads after close' do
    closed_input = Vajra::NativeInput.from_string('x')
    closed_input.close
    expect { closed_input.read }.to raise_error(IOError, 'rack.input is closed')
  end

  it 'keeps the private request body string guard strict' do
    expect { described_class.send(:binary_request_body, nil) }
      .to raise_error(TypeError, 'request_body must be a String')
  end

  it 'rejects invalid native request input reads' do
    input = Vajra::NativeInput.from_string('')

    expect { input.read(-1) }.to raise_error(ArgumentError, 'negative length')
    expect { input.gets('') }.to raise_error(ArgumentError, 'separator cannot be empty')
  end

  it 'removes the old Ruby StreamingInput implementation' do
    expect(described_class.const_defined?(:StreamingInput, false)).to be(false)
  end

  it 'does not swallow close errors from the response body' do
    closing_body = Class.new do
      def each
        yield 'OK'
      end

      def close
        nil.missing_method
      end
    end.new

    described_class.install!(->(_env) { [200, {}, closing_body] })

    expect { described_class.call([%w[REQUEST_METHOD GET]], ''.b) }.to raise_error(NoMethodError)
  end

  it 'closes the response body when header normalization raises' do
    closing_body = Class.new do
      def initialize
        @close_calls = 0
      end

      attr_reader :close_calls

      def each
        yield 'OK'
      end

      def close
        @close_calls += 1
      end
    end.new

    headers = Class.new do
      def each
        raise 'headers exploded'
      end
    end.new

    described_class.install!(->(_env) { [200, headers, closing_body] })

    expect { described_class.call([%w[REQUEST_METHOD GET]], ''.b) }.to raise_error(RuntimeError, 'headers exploded')
    expect(closing_body.close_calls).to eq(1)
  end

  it 'does not swallow close errors when header normalization raises' do
    closing_body = Class.new do
      def each
        yield 'OK'
      end

      def close
        raise 'close exploded'
      end
    end.new

    headers = Class.new do
      def each
        raise 'headers exploded'
      end
    end.new

    described_class.install!(->(_env) { [200, headers, closing_body] })

    expect { described_class.call([%w[REQUEST_METHOD GET]], ''.b) }.to raise_error(RuntimeError, 'close exploded')
  end

  it 'closes the response body when status normalization raises' do
    closing_body = Class.new do
      def each
        yield 'OK'
      end

      def close
        @closed = true
      end

      def closed?
        @closed == true
      end
    end.new

    described_class.install!(->(_env) { ['bad-status', {}, closing_body] })

    expect { described_class.call([%w[REQUEST_METHOD GET]], ''.b) }.to raise_error(ArgumentError)
    expect(closing_body.closed?).to be(true)
  end

  it 'closes the response body exactly once when body collection raises' do
    closing_body = Class.new do
      def initialize
        @close_calls = 0
      end

      attr_reader :close_calls

      def each
        raise 'body exploded'
      end

      def close
        @close_calls += 1
      end
    end.new

    described_class.install!(->(_env) { [200, {}, closing_body] })

    expect { described_class.call([%w[REQUEST_METHOD GET]], ''.b) }.to raise_error(RuntimeError, 'body exploded')
    expect(closing_body.close_calls).to eq(1)
  end

  it 'does not swallow close errors when body collection raises' do
    closing_body = Class.new do
      def each
        raise 'body exploded'
      end

      def close
        raise 'close exploded'
      end
    end.new

    described_class.install!(->(_env) { [200, {}, closing_body] })

    expect { described_class.call([%w[REQUEST_METHOD GET]], ''.b) }.to raise_error(RuntimeError, 'close exploded')
  end
end

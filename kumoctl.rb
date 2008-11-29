require 'rubygems' rescue LoadError nil
require 'msgpack'
require 'socket'


class KumoManager
	def initialize(host, port)
		@sock = TCPSocket.open(host, port)
		@pk = MessagePack::Unpacker.new
		@buffer = ''
		@nread = 0
		@seqid = rand(1<<16)  # FIXME 1 << 32
		@callback = {}
	end


	private
	def send_request(seq, cmd, param)
		@sock.write [true, seq, cmd, param].to_msgpack
		@sock.flush
	rescue
		@sock.close
		raise
	end

	def receive_message
		while true
			if @buffer.length > @nread
				@nread = @pk.execute(@buffer, @nread)
				if @pk.finished?
					msg = @pk.data
					@pk.reset
					@buffer.slice!(0, @nread)
					@nread = 0
					if msg[0]
						process_request(msg[1], msg[2], msg[3])
					else
						process_response(msg[1], msg[3], msg[2])
					end
					return msg[1]
				end
			end
			@buffer << @sock.sysread(1024)
		end
	end

	def process_request(seqid, cmd, param)
		raise "request received, excpect response"
	end

	def process_response(seqid, res, err)
		if cb = @callback[seqid]
			cb.call(res, err)
		end
	end

	def synchronize_response(seqid)
		while receive_message != seqid; end
	end

	def send_request_async(cmd, param, &callback)
		seqid = @seqid
		# FIXME 1 << 32
		@seqid += 1; if @seqid >= 1<<16 then @seqid = 0 end
		@callback[seqid] = callback if callback
		send_request(seqid, cmd, param)
		seqid
	end

	def send_request_sync(cmd, param)
		res = nil
		err = nil
		seqid = send_request_async(cmd, param) {|rres, rerr|
			res = rres
			err = rerr
		}
		synchronize_response(seqid)
		return [res, err]
	end

	def send_request_sync_ex(cmd, param)
		res, err = send_request_sync(cmd, param)
		raise "error #{err}" if err
		res
	end


	def rpc_addr(raw)
		if raw.length == 6
			addr = Socket.pack_sockaddr_in(0, '0.0.0.0')
			addr[2,6] = raw[0,6]
		else
			addr = Socket.pack_sockaddr_in(0, '::')
			addr[2,2]  = raw[0,2]
			addr[8,20] = raw[2,20]
		end
		Socket.unpack_sockaddr_in(addr).reverse
	end

	public
	def GetStatus
		res = send_request_sync_ex(84, [])
		form = {}
		nodes = res[0]

		clocktime = nodes.slice!(-1)
		date = Time.at(clocktime >> 32)
		clock = clocktime & ((1<<32)-1)

		nodes.each {|nodes|
			nodes.map! {|raw|
				active = (raw.slice!(0) == "\1"[0])
				rpc_addr(raw) << active
			}
		}

		newcomers = res[1]
		res[1].map! {|raw|
			rpc_addr(raw)
		}

		return [nodes, newcomers, date, clock]
	end

	def AttachNewServers
		send_request_sync_ex(85, [])
	end

	def DetachFaultServers
		res = send_request_sync(86, [])
	end

	def CreateBackup(suffix)
		res = send_request_sync(87, [suffix])
	end

	CONTROL_DEFAULT_PORT = 19799

end

$now = Time.now.strftime("%Y%m%d")

def usage
	puts "Usage: #{$0} address[:port=#{KumoManager::CONTROL_DEFAULT_PORT}] command [options]"
	puts "command:"
	puts "   stat                       get status"
	puts "   attach                     attach all new servers and start replace"
	puts "   detach                     detach all fault servers and start replace"
	puts "   backup  [suffix=#{$now }]  create backup with specified suffix"
	exit 1
end

if ARGV.length < 2
	usage
end

addr = ARGV.shift
host, port = addr.split(':', 2)
port ||= KumoManager::CONTROL_DEFAULT_PORT

cmd = ARGV.shift
case cmd
when "stat"
	usage if ARGV.length != 0
	joined, not_joined, date, clock =
			KumoManager.new(host, port).GetStatus
	puts "hash space timestamp:"
	puts "  #{date} clock #{clock}"
	puts "joined node:"
	joined.each {|addr, port, active|
		puts "  #{addr}:#{port}  (#{active ? "active":"fault"})"
	}
	puts "not joined node:"
	not_joined.each {|addr, port|
		puts "  #{addr}:#{port}"
	}

when "attach"
	usage if ARGV.length != 0
	p KumoManager.new(host, port).AttachNewServers

when "detach"
	usage if ARGV.length != 0
	p KumoManager.new(host, port).DetachFaultServers

when "backup"
	if ARGV.length == 0
		suffix = $now
	elsif ARGV.length == 1
		suffix = ARGV.shift
	else
		usage
	end
	puts "suffix=#{suffix}"
	p KumoManager.new(host, port).CreateBackup(suffix)

else
	puts "unknown command #{cmd}"
	puts ""
	usage
end


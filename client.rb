# Sample program for listening to AQ / Temp / RH information.
#
# The server that runs on the ESP8266 broadcasts data via UDP on port 9000.
# This program just listens for that data and decodes it

require "socket"
require "ipaddr"
require "json"
require "influxdb"

MULTICAST_ADDR = "224.0.0.1"
BIND_ADDR = "0.0.0.0"
PORT = 9000

AQ = Struct.new(:pm1_0_standard, :pm2_5_standard, :pm10_standard,
                :pm1_0_env,      :pm2_5_env,
                :concentration_unit,

                # These fields are "number of particles beyond N um
                # per 0.1L of air". These numbers are multiplied by
                # 10, so 03um == "number of particles beyond 0.3um
                # in 0.1L of air"
                :particle_03um,   :particle_05um,   :particle_10um,
                :particle_25um,   :particle_50um,   :particle_100um)

TempRH = Struct.new(:temp, :rh)

class Combined
  attr_reader :time, :mac, :record_id, :aq, :temprh

  def initialize time, mac, record_id, aq, temprh
    @time      = time
    @mac       = mac
    @record_id = record_id
    @aq        = aq
    @temprh    = temprh
  end
end

if_addr = Socket.getifaddrs.find { |s| s.addr.ipv4? && !s.addr.ipv4_loopback? }
p if_addr.addr.ip_address

socket = UDPSocket.new
membership = IPAddr.new(MULTICAST_ADDR).hton + IPAddr.new(BIND_ADDR).hton

socket.setsockopt(:IPPROTO_IP, :IP_ADD_MEMBERSHIP, membership)
socket.setsockopt(:IPPROTO_IP, :IP_MULTICAST_TTL, 1)
socket.setsockopt(:SOL_SOCKET, :SO_REUSEPORT, 1)

socket.bind(BIND_ADDR, PORT)
client = InfluxDB::Client.new 'testing', host: 'grafana.internal.ddelnano.com'

loop do
  m, _ = socket.recvfrom(2000)
  record = JSON.load(m)

  data = record["aq"].unpack("m0").first
  unpack = data.unpack('CCnn14')
  crc = 0x42 + 0x4d + 28 + data.bytes.drop(4).first(26).inject(:+)
  unless crc != unpack.last
    aq = AQ.new(*unpack.drop(3).first(12))
    temprh = TempRH.new(record["temperature"] / 100.0, record["humidity"] / 100.0)
    rec = Combined.new(Time.now.utc, record["mac"], record["record_id"], aq, temprh)

    tags = {
      mac: record['mac'],
    }
    data = [
      {
        series: 'aqi',
        tags: tags,
        values: {
          pm1_0: aq.pm1_0_standard,
          pm2_5: aq.pm2_5_standard,
        },
      },
      {
        series: 'temperature',
        tags: tags,
        values: {
          value: record['temperature'],
        },

      },
    ]
    client.write_points(data, 's')
    p rec
  end
end


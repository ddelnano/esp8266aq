require "uart"
require "io/wait"
require "esp_client"

begin
  esp = ESPClient.open
  esp.gp2 = 1
  esp.gp3 = 0

  trap "INFO" do
    puts "hi"
    esp.gp3 ^= 1
    esp.gp2 ^= 1
  end

  UART.open '/dev/cu.usbmodem1441101' do |serial|
    loop do
      serial.wait_readable
      puts serial.readline
    end
  end

ensure
  puts "turning gp2 off"
  esp.gp2 = 0
end

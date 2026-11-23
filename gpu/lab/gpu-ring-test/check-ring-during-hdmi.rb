require 'dl'

$stdout.sync = true
cg = DL.dlopen('/System/Library/Frameworks/ApplicationServices.framework/Frameworks/CoreGraphics.framework/CoreGraphics')
display = cg.sym('CGMainDisplayID', 'I').call[0]
abort 'No Quartz primary display' if display == 0

def dimensions(cg, display)
  width = cg.sym('CGDisplayPixelsWide', 'II').call(display)[0]
  height = cg.sym('CGDisplayPixelsHigh', 'II').call(display)[0]
  [width, height]
end

def exact_mode(cg, display, width, height)
  exact = DL.malloc(4)
  exact[0, 4] = [0].pack('I')
  mode = cg.sym('CGDisplayBestModeForParameters', 'PIIIIP').call(display, 32, width, height, exact)[0]
  raise "No exact mode for #{width}x#{height}" if mode.to_i == 0 || exact[0, 4].unpack('I')[0] == 0
  mode
end

def switch_mode(cg, display, mode)
  slot = DL.malloc(4)
  status = cg.sym('CGBeginDisplayConfiguration', 'IP').call(slot)[0]
  raise "CGBeginDisplayConfiguration failed (#{status})" unless status == 0

  config = DL::PtrData.new(slot[0, 4].unpack('I')[0])
  status = cg.sym('CGConfigureDisplayMode', 'IPIP').call(config, display, mode)[0]
  if status != 0
    cg.sym('CGCancelDisplayConfiguration', 'IP').call(config)
    raise "CGConfigureDisplayMode failed (#{status})"
  end

  # kCGConfigureForAppOnly: Quartz can restore the session when this process exits.
  status = cg.sym('CGCompleteDisplayConfiguration', 'IPI').call(config, 0)[0]
  raise "CGCompleteDisplayConfiguration failed (#{status})" unless status == 0
end

def verify_mode(cg, display, expected)
  actual = nil
  12.times do
    actual = dimensions(cg, display)
    if actual == expected
      script = 'tell application "Finder" to get bounds of window of desktop'
      bounds = IO.popen("/usr/bin/osascript -e '#{script}'", 'r') { |out| out.read.strip }
      puts "Quartz #{actual.join('x')}, Finder bounds #{bounds}"
      raise "Finder did not confirm #{expected.join('x')}" unless bounds == "0, 0, #{expected.join(', ')}"
      return
    end
    sleep 0.25
  end
  raise "Expected #{expected.join('x')}, Quartz reports #{actual.join('x')}"
end

def itunes(script)
  commands = ['tell application "iTunes"'] + script.split("\n") + ['end tell']
  args = commands.map { |line| ['-e', line] }.flatten
  result = IO.popen('-', 'r') do |out|
    if out
      out.read.strip
    else
      exec('/usr/bin/osascript', *args)
    end
  end
  raise 'iTunes command failed' unless $?.success?
  result
end

def check_playback(previous_position = nil)
  state, position = itunes('return {player state, player position}').split(', ')
  raise 'iTunes stopped playing' unless state == 'playing'
  position = Float(position)
  if previous_position && position <= previous_position
    raise 'iTunes timeline did not advance'
  end
  position
end

desktop1080 = [1920, 1080]
desktop720 = [1280, 720]
original_dimensions = dimensions(cg, display)
abort 'Unexpected starting display mode' unless [desktop1080, desktop720].include?(original_dimensions)
other_dimensions = original_dimensions == desktop1080 ? desktop720 : desktop1080
original_mode = exact_mode(cg, display, *original_dimensions)
other_mode = exact_mode(cg, display, *other_dimensions)

audio_state = itunes('return player state')
original_position = itunes('return player position') if audio_state == 'paused'
raise 'iTunes is muted' unless itunes('return (not mute and sound volume is greater than 0)') == 'true'
puts "TEST_START=#{Time.now.strftime('%Y-%m-%d %H:%M:%S')}"
begin
  case audio_state
  when 'stopped'
    itunes('play (first file track of library playlist 1 whose duration is greater than 100 and duration is less than 900)')
  when 'paused'
    itunes('if (duration of current track) - player position < 75 then set player position to 0
play')
  when 'playing'
    # Leave an already-playing track running.
  else
    raise "Unexpected iTunes state #{audio_state}"
  end

  sleep 6
  last_position = check_playback
  puts "iTunes playing at #{last_position.to_i}s in #{original_dimensions.join('x')}"
  3.times do |pass|
    puts "Round trip #{pass + 1}"
    [[other_dimensions, other_mode], [original_dimensions, original_mode]].each do |expected, mode|
      switch_mode(cg, display, mode)
      verify_mode(cg, display, expected)
      sleep 6
      last_position = check_playback(last_position)
      puts "#{Time.now.strftime('%H:%M:%S')} iTunes playing at #{last_position.to_i}s in #{expected.join('x')}"
    end
  end
ensure
  begin
    if dimensions(cg, display) != original_dimensions
      switch_mode(cg, display, original_mode)
      verify_mode(cg, display, original_dimensions)
    end
  ensure
    if audio_state == 'stopped'
      itunes('stop')
    elsif audio_state == 'paused'
      itunes("pause\nset player position to #{Float(original_position)}")
    end
  end
end
puts "TEST_END=#{Time.now.strftime('%Y-%m-%d %H:%M:%S')}"
puts "Final display #{dimensions(cg, display).join('x')}, iTunes #{itunes('return player state')}"

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

def play_clip
  script = <<'APPLESCRIPT'
tell application "iTunes"
    if player state is playing then return "iTunes was already playing"
    if player state is not paused or not (exists current track) then error "No paused track to sample"
    if mute or sound volume is 0 then error "iTunes is muted"
    set originalPosition to player position
    try
        if (duration of current track) - originalPosition < 6 then set player position to 0
        set startPosition to player position
        play
        delay 4
        set elapsedPosition to (player position) - startPosition
        pause
        set player position to originalPosition
        if elapsedPosition < 1 then error "iTunes timeline did not advance"
        return "iTunes playback advanced " & (elapsedPosition as text) & " seconds and returned to paused"
    on error messageText number errorNumber
        pause
        set player position to originalPosition
        error messageText number errorNumber
    end try
end tell
APPLESCRIPT
  args = script.split("\n").map { |line| ['-e', line] }.flatten
  raise 'iTunes sample failed' unless system('/usr/bin/osascript', *args)
end

full = [1920, 1080]
native720 = [1280, 720]
full_mode = exact_mode(cg, display, *full)
native720_mode = exact_mode(cg, display, *native720)
verify_mode(cg, display, full)
puts 'Playing the iTunes sample in 1080p'
sleep 3
play_clip
begin
  switch_mode(cg, display, native720_mode)
  sleep 2
  verify_mode(cg, display, native720)
  puts 'Playing the same iTunes sample in 720p'
  sleep 3
  play_clip
  sleep 3
ensure
  if dimensions(cg, display) != full
    switch_mode(cg, display, full_mode)
    sleep 2
    verify_mode(cg, display, full)
  end
end

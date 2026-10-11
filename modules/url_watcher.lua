-- url_watcher.lua — watches shell output for URLs and logs them.
-- Demonstrates: on("output") stream hook. Run `terminal` from another
-- terminal to see the log lines on stderr.

local seen = {}

terminal.on("output", function(chunk)
  for url in chunk:gmatch("https?://[%w%.%-_/%%?=&#:+~]+") do
    if not seen[url] then
      seen[url] = true
      terminal.log("url: " .. url)
    end
  end
end)

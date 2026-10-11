-- keybinds.lua — example custom keybindings.
-- Demonstrates: terminal.bind, terminal.send

-- cmd+l: clear scrollback + screen (sends ctrl-l to the shell)
terminal.bind("cmd+l", function()
  terminal.send("\12")
end)

-- cmd+g: type a git status for you
terminal.bind("cmd+g", function()
  terminal.send("git status\r")
end)

cask "terminal" do
  version "0.1.0"
  sha256 "2df5fd02c8c0ccfeff49d234e1769815cee35d80b011d6af8c598e4aac32f90e"

  url "https://github.com/fintermxyz/terminal/releases/download/v#{version}/terminal.dmg"
  name "terminal"
  desc "GPU-accelerated terminal with LuaJIT modules and built-in file viewers"
  homepage "https://github.com/fintermxyz/terminal"

  depends_on arch: :arm64
  depends_on macos: :monterey

  app "terminal.app"

  zap trash: "~/.config/terminal"

  caveats <<~EOS
    terminal is not notarized yet. If macOS blocks it on first launch, open
    System Settings → Privacy & Security and click "Open Anyway".
  EOS
end

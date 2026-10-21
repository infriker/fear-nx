// Installer messages. Adapted from nfsmw-nx-installer (GPL-3.0).
const TEXTS = {
  "platform": "NINTENDO SWITCH · NATIVE PORT",
  "headline": "Bring your own fear.",
  "lead": "Create a Switch package from your own Xbox 360 copy of F.E.A.R. Your game files stay on this computer.",
  "privacy": "Local processing. No uploads. No account.",
  "step1": "Choose your game",
  "formatIso": "Disc image (.iso)",
  "formatXex": "Game folder",
  "formatHint": "Select an Xbox 360 ISO, or the extracted folder containing default.xex and Game/.",
  "nothingChosen": "Nothing selected yet.",
  "chosenFile": "{name} ({size})",
  "chosenFolder": "{name}: {count} files ({size})",
  "chosenFolderOne": "{name}: 1 file ({size})",
  "step2": "Create your package",
  "step2First": "First install: includes the port and your game files.",
  "create": "Create fear-nx.zip",
  "step2Update": "Already installed? Download only the new program, settings and notices.",
  "createUpdate": "Create update ZIP",
  "step3": "Copy to your Switch",
  "step3Extract": "Extract the ZIP into <code>sdmc:/switch/</code>.",
  "step3Start": "Hold <strong>R</strong> while launching an installed game, then open <code>fear_nx.nro</code> from the Homebrew Menu. Album/applet mode is unsupported.",
  "updateHint": "Keep your game files and userdata folder. Back up custom settings before updating.",
  "fullGuide": "Installation guide and controls →",
  "checking": "Checking your game…",
  "supported": "Supported Xbox 360 executable and game data folder found.",
  "executableOnly": "Supported executable. Game/uberCatalog.cat is missing; only the update package is available.",
  "wrongRelease": "This is F.E.A.R. for Xbox 360, but its executable is not the supported revision. Include this hash when reporting a compatibility issue:",
  "notGame": "This is not a supported F.E.A.R. Xbox 360 executable.",
  "gameName": "F.E.A.R. — Xbox 360",
  "notIso": "Not a readable Xbox 360 ISO",
  "notComplete": "Game files could not be verified",
  "noExecutable": "default.xex was not found. Choose the ISO or the folder containing default.xex and Game/.",
  "incomplete": "Game/uberCatalog.cat is missing. Select the full extracted game or its disc image.",
  "notSupported": "This executable revision is not supported (SHA-256 {hash}).",
  "downloadingBuild": "Downloading F.E.A.R. NX…",
  "downloadFailed": "Could not download {url} (HTTP {status}). Check your connection and try again.",
  "buildMismatch": "The downloaded program does not match the release. Reload this page and try again.",
  "logRelease": "Your copy: F.E.A.R., {edition}.",
  "logUpdate": "Update package: original game files are not copied.",
  "readingFiles": "Reading your game…",
  "copied": "Copied {path}",
  "done": "Package complete.",
  "noStreaming": "This window cannot stream a large download. Use a regular desktop Chrome, Edge or Firefox window with service workers enabled.",
  "downloadStarted": "Download started ({size}). Keep this page open until it finishes.",
  "finished": "Finished in {seconds} s.",
  "error": "Error: {message}",
  "cancelled": "The download was cancelled.",
  "sourceCode": "Source code",
  "installerSource": "Installer source",
  "releaseLink": "Latest release",
  "legal": "An unofficial fan project. You need your own copy of the Xbox 360 game. F.E.A.R. and its marks belong to their respective owners."
};

export function t(key, vars = {}) {
  return (TEXTS[key] ?? key).replace(/\{(\w+)\}/g,
    (match, name) => name in vars ? String(vars[name]) : match);
}

export function formatSize(bytes) {
  const units = ['B', 'KB', 'MB', 'GB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit++; }
  return `${value.toFixed(unit ? 1 : 0)} ${units[unit]}`;
}

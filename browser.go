package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

// openBrowser tries to open the given URL in a borderless "app" window — no
// address bar, no tabs, no bookmarks bar, so it reads as a desktop app
// instead of a website. Same trick as greenies: Chromium/Chrome's --app flag,
// pointed at a dedicated profile directory so window placement persists
// across launches without wilson needing to track it itself, and so it never
// touches the user's normal browser profile.
//
// Falls back to the system's default browser (full chrome) if no
// Chromium-family browser is found, or does nothing if that fails too — the
// terminal always prints the URL, so a failed auto-open is never a dead end.
func openBrowser(url string) {
	home, _ := os.UserHomeDir()
	profileDir := filepath.Join(home, ".wilson", "chromium-profile")

	// A fresh profile hasn't saved a window size yet, so give it a sensible
	// starting size; once the profile exists, Chromium remembers the size the
	// user last left it at and passing our own would just override that.
	firstRun := true
	if _, err := os.Stat(profileDir); err == nil {
		firstRun = false
	}

	appArgs := func(browser string) []string {
		args := []string{"--app=" + url, "--user-data-dir=" + profileDir}
		if firstRun {
			args = append(args, "--window-size=1280,900")
		}
		return args
	}

	switch runtime.GOOS {
	case "linux":
		for _, browser := range []string{"chromium-browser", "chromium", "google-chrome", "google-chrome-stable"} {
			if path, err := exec.LookPath(browser); err == nil {
				_ = exec.Command(path, appArgs(browser)...).Start()
				return
			}
		}
		_ = exec.Command("xdg-open", url).Start()
	case "darwin":
		if path, err := exec.LookPath("google-chrome"); err == nil {
			_ = exec.Command(path, appArgs("google-chrome")...).Start()
			return
		}
		for _, chromeApp := range []string{
			"/Applications/Google Chrome.app",
			"/Applications/Chromium.app",
		} {
			if _, err := os.Stat(chromeApp); err == nil {
				args := append([]string{"-a", chromeApp, "--args"}, appArgs("")...)
				_ = exec.Command("open", args...).Start()
				return
			}
		}
		_ = exec.Command("open", url).Start()
	case "windows":
		for _, chromePath := range []string{
			os.Getenv("ProgramFiles") + `\Google\Chrome\Application\chrome.exe`,
			os.Getenv("ProgramFiles(x86)") + `\Google\Chrome\Application\chrome.exe`,
			os.Getenv("LocalAppData") + `\Google\Chrome\Application\chrome.exe`,
		} {
			if _, err := os.Stat(chromePath); err == nil {
				_ = exec.Command(chromePath, appArgs("")...).Start()
				return
			}
		}
		_ = exec.Command("rundll32", "url.dll,FileProtocolHandler", url).Start()
	}
}

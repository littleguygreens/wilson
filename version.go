package main

import (
	"runtime/debug"
	"time"
)

// version identifies the running build from the VCS info Go's toolchain
// embeds automatically (since 1.18) when building inside a git checkout.
// There's no version number to remember to bump: across a long string of
// small fixes, every build just reports the commit it was built from, so
// "did my pull/build actually pick this up" is always answerable by eye
// (via -version or the -serve startup line) instead of by digging through
// git log or binary timestamps.
//
// "unknown" only happens building from a source tree with no VCS info at
// all (e.g. a tarball export, not a git clone) -- go build always tries to
// embed it when .git is present, so this is a "was this ever a git
// checkout" fallback, not something a normal build hits.
func version() string {
	info, ok := debug.ReadBuildInfo()
	if !ok {
		return "unknown"
	}

	var revision, commitTime string
	dirty := false
	for _, s := range info.Settings {
		switch s.Key {
		case "vcs.revision":
			revision = s.Value
		case "vcs.time":
			commitTime = s.Value
		case "vcs.modified":
			dirty = s.Value == "true"
		}
	}
	if revision == "" {
		return "unknown"
	}
	if len(revision) > 9 {
		revision = revision[:9]
	}

	v := revision
	if t, err := time.Parse(time.RFC3339, commitTime); err == nil {
		v += " (" + t.Format("2006-01-02") + ")"
	}
	if dirty {
		// Built from a tree with uncommitted changes on top of that commit --
		// flagged loudly since it means the binary isn't fully explained by git
		// log, e.g. a fix that was tested but not yet committed.
		v += " +uncommitted changes"
	}
	return v
}

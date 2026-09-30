package main

import (
	"bytes"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func newServer(t *testing.T) *server {
	t.Helper()
	return &server{dir: t.TempDir(), token: []byte("s3cret"), max: 1024}
}

func post(s *server, path, token string, body []byte) *httptest.ResponseRecorder {
	r := httptest.NewRequest(http.MethodPost, path, bytes.NewReader(body))
	r.Header.Set("X-OTA-Token", token)
	r.Header.Set("X-Device-Hostname", "WS2812BCLOCK")
	r.Header.Set("X-Firmware-Version", "0.1.8")
	w := httptest.NewRecorder()
	s.handleCrash(w, r)
	return w
}

func TestStoresDump(t *testing.T) {
	s := newServer(t)
	w := post(s, "/crash/ws2812bclock-esp32", "s3cret", []byte("\x7fELFcoredump"))
	if w.Code != http.StatusCreated {
		t.Fatalf("got %d, want 201: %s", w.Code, w.Body)
	}

	matches, _ := filepath.Glob(filepath.Join(s.dir, "ws2812bclock-esp32", "WS2812BCLOCK-0.1.8-*.elf"))
	if len(matches) != 1 {
		t.Fatalf("expected one stored dump, found %v", matches)
	}
	got, err := os.ReadFile(matches[0])
	if err != nil || string(got) != "\x7fELFcoredump" {
		t.Fatalf("stored content wrong: %q, %v", got, err)
	}
}

func TestRejectsBadToken(t *testing.T) {
	s := newServer(t)
	if w := post(s, "/crash/ws2812bclock-esp32", "wrong", []byte("data")); w.Code != http.StatusForbidden {
		t.Fatalf("got %d, want 403", w.Code)
	}
	// Nothing should have been written, not even a temp file.
	entries, _ := os.ReadDir(s.dir)
	if len(entries) != 0 {
		t.Fatalf("rejected upload left %d entries behind", len(entries))
	}
}

// The model comes straight off the URL, so it is the obvious way to try to
// write outside the storage directory.
func TestRejectsPathTraversal(t *testing.T) {
	for _, model := range []string{"../etc", "a/b", "..", "", strings.Repeat("x", 65)} {
		s := newServer(t)
		if w := post(s, "/crash/"+model, "s3cret", []byte("data")); w.Code != http.StatusBadRequest {
			t.Errorf("model %q: got %d, want 400", model, w.Code)
		}
	}
}

func TestRejectsOversizedBody(t *testing.T) {
	s := newServer(t)
	if w := post(s, "/crash/ws2812bclock-esp32", "s3cret", bytes.Repeat([]byte("x"), 2048)); w.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("got %d, want 413", w.Code)
	}
	matches, _ := filepath.Glob(filepath.Join(s.dir, "ws2812bclock-esp32", "*.elf"))
	if len(matches) != 0 {
		t.Fatalf("oversized upload was stored: %v", matches)
	}
}

func TestRejectsEmptyBody(t *testing.T) {
	s := newServer(t)
	if w := post(s, "/crash/ws2812bclock-esp32", "s3cret", nil); w.Code != http.StatusBadRequest {
		t.Fatalf("got %d, want 400", w.Code)
	}
}

// Unsafe header values must not reach the filename; they fall back to defaults.
func TestSanitisesHeaders(t *testing.T) {
	s := newServer(t)
	r := httptest.NewRequest(http.MethodPost, "/crash/ws2812bclock-esp32", strings.NewReader("data"))
	r.Header.Set("X-OTA-Token", "s3cret")
	r.Header.Set("X-Device-Hostname", "../../etc/passwd")
	r.Header.Set("X-Firmware-Version", "0.1.8; rm -rf /")
	w := httptest.NewRecorder()
	s.handleCrash(w, r)

	if w.Code != http.StatusCreated {
		t.Fatalf("got %d, want 201", w.Code)
	}
	matches, _ := filepath.Glob(filepath.Join(s.dir, "ws2812bclock-esp32", "unknown-unknown-*.elf"))
	if len(matches) != 1 {
		t.Fatalf("unsafe headers were not replaced with defaults: %v", matches)
	}
}

func TestRejectsGet(t *testing.T) {
	s := newServer(t)
	r := httptest.NewRequest(http.MethodGet, "/crash/ws2812bclock-esp32", nil)
	r.Header.Set("X-OTA-Token", "s3cret")
	w := httptest.NewRecorder()
	s.handleCrash(w, r)
	if w.Code != http.StatusMethodNotAllowed {
		t.Fatalf("got %d, want 405", w.Code)
	}
}

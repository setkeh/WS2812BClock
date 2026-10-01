// crashd receives ESP32 core dumps from the clocks and writes them to disk.
//
// It exists because a core dump is an opaque binary that has to arrive intact,
// which rules out the lossy syslog path the clocks use for everything else.
// It deliberately does not use a general-purpose upload module: this endpoint
// is reachable from the network the devices are on, and the only thing it is
// allowed to do is
// create one new file per crash, under a name it chooses itself.
//
//	POST /crash/{model}
//	  X-OTA-Token         shared token, same one the firmware sends for updates
//	  X-Device-Hostname   which clock (optional, defaults to "unknown")
//	  X-Firmware-Version  what it was running (optional)
//	  X-App-Elf-Sha256    identifies the build whose symbols decode this dump
//	  body                the raw coredump partition, verbatim
//
// Stored as {dir}/{model}/{hostname}-{version}-{unix}.elf
package main

import (
	"crypto/subtle"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

// Set at build time with -ldflags "-X main.version=...", so a deployed binary
// can say what it is.
var version = "dev"

// Everything that reaches a filesystem path is forced through this. The
// server picks the filename, but these still come from the network.
//
// The leading character must be alphanumeric, and that is load-bearing rather
// than tidiness: allowing a leading dot would admit ".." -- which passes a
// naive character-class check, and which filepath.Join happily resolves to the
// parent directory, putting an upload outside the storage tree entirely. It
// also keeps uploads from colliding with the ".upload-*" temporary files.
var safe = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`)

type server struct {
	dir   string
	token []byte
	max   int64
}

func main() {
	addr := flag.String("addr", envOr("CRASHD_ADDR", "127.0.0.1:8099"), "listen address")
	dir := flag.String("dir", envOr("CRASHD_DIR", "/srv/crash"), "directory to write dumps into")
	tokenFile := flag.String("token-file", envOr("CRASHD_TOKEN_FILE", ""), "file holding the shared token")
	max := flag.Int64("max-bytes", 128*1024, "largest accepted dump")
	flag.Parse()

	// The coredump partition is 64 KB, so anything approaching twice that is
	// not a dump. Refusing early keeps a bad actor from streaming to disk.
	if *tokenFile == "" {
		log.Fatal("crashd: -token-file is required; this endpoint is reachable from the device network")
	}
	raw, err := os.ReadFile(*tokenFile)
	if err != nil {
		log.Fatalf("crashd: cannot read token file: %v", err)
	}
	token := []byte(strings.TrimSpace(string(raw)))
	if len(token) == 0 {
		log.Fatalf("crashd: token file %s is empty", *tokenFile)
	}
	if err := os.MkdirAll(*dir, 0o750); err != nil {
		log.Fatalf("crashd: cannot create %s: %v", *dir, err)
	}

	s := &server{dir: *dir, token: token, max: *max}

	mux := http.NewServeMux()
	mux.HandleFunc("/crash/", s.handleCrash)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintln(w, "ok")
	})

	srv := &http.Server{
		Addr:              *addr,
		Handler:           mux,
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       5 * time.Minute, // a clock on a weak link is slow
		WriteTimeout:      30 * time.Second,
	}
	log.Printf("crashd %s: listening on %s, writing to %s", version, *addr, *dir)
	log.Fatal(srv.ListenAndServe())
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func (s *server) handleCrash(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}

	// Compared in constant time so a wrong token cannot be found a byte at a
	// time by timing the response.
	got := []byte(r.Header.Get("X-OTA-Token"))
	if subtle.ConstantTimeCompare(got, s.token) != 1 {
		log.Printf("crashd: rejected upload from %s: bad token", r.RemoteAddr)
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}

	model := strings.TrimPrefix(r.URL.Path, "/crash/")
	if !safe.MatchString(model) {
		http.Error(w, "bad model", http.StatusBadRequest)
		return
	}

	host := field(r.Header.Get("X-Device-Hostname"), "unknown")
	version := field(r.Header.Get("X-Firmware-Version"), "unknown")
	elfSHA := field(r.Header.Get("X-App-Elf-Sha256"), "")

	modelDir := filepath.Join(s.dir, model)
	if err := os.MkdirAll(modelDir, 0o750); err != nil {
		log.Printf("crashd: cannot create %s: %v", modelDir, err)
		http.Error(w, "storage error", http.StatusInternalServerError)
		return
	}

	// The server names the file. Nothing a client sends is used as a path
	// element without passing `safe` first, and the timestamp makes repeated
	// crashes from one device distinct rather than overwriting.
	name := fmt.Sprintf("%s-%s-%d.elf", host, version, time.Now().Unix())
	final := filepath.Join(modelDir, name)

	// Written to a temporary file and renamed, so a dump that arrives
	// half-way never looks like a complete one to whoever reads the directory.
	tmp, err := os.CreateTemp(modelDir, ".upload-*")
	if err != nil {
		log.Printf("crashd: cannot create temp file: %v", err)
		http.Error(w, "storage error", http.StatusInternalServerError)
		return
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName) // no-op once the rename has succeeded

	n, err := io.Copy(tmp, http.MaxBytesReader(w, r.Body, s.max))
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		var tooLarge *http.MaxBytesError
		if errors.As(err, &tooLarge) {
			log.Printf("crashd: %s sent more than %d bytes", r.RemoteAddr, s.max)
			http.Error(w, "too large", http.StatusRequestEntityTooLarge)
			return
		}
		log.Printf("crashd: upload from %s failed after %d bytes: %v", r.RemoteAddr, n, err)
		http.Error(w, "upload failed", http.StatusBadRequest)
		return
	}
	if n == 0 {
		http.Error(w, "empty body", http.StatusBadRequest)
		return
	}

	if err := os.Chmod(tmpName, 0o640); err != nil {
		log.Printf("crashd: chmod %s: %v", tmpName, err)
	}
	if err := os.Rename(tmpName, final); err != nil {
		log.Printf("crashd: cannot store %s: %v", final, err)
		http.Error(w, "storage error", http.StatusInternalServerError)
		return
	}

	log.Printf("crashd: stored %s (%d bytes) from %s, app_elf_sha256=%q", final, n, r.RemoteAddr, elfSHA)

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	_ = json.NewEncoder(w).Encode(map[string]any{
		"stored": filepath.Join(model, name),
		"bytes":  n,
	})
}

// field accepts a header only if it is safe to put in a filename.
func field(v, def string) string {
	v = strings.TrimSpace(v)
	if !safe.MatchString(v) {
		return def
	}
	return v
}

// SPDX-License-Identifier: GPL-3.0-only
package main

import (
	"context"
	"crypto/subtle"
	"embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

//go:embed web/*
var webAssets embed.FS

type Server struct {
	store        *Store
	media, stage string
	allowed      []*net.IPNet
	uploadMu     sync.Mutex
	uploadLocks  map[string]*sync.Mutex
	loginMu      sync.Mutex
	loginURL     string
	loginBusy    bool
}

const chunkLimit int64 = 8 << 20

func env(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}

func main() {
	state := env("MB_STATE_DIR", "./state")
	media := env("MB_MEDIA_DIR", "./media")
	if err := os.MkdirAll(state, 0700); err != nil {
		log.Fatal(err)
	}
	if err := os.MkdirAll(media, 0770); err != nil {
		log.Fatal(err)
	}
	stage := filepath.Join(media, ".mediabridge-staging")
	if err := os.MkdirAll(stage, 0770); err != nil {
		log.Fatal(err)
	}
	// Jellyfin treats a directory containing .ignore as excluded from scans.
	if err := os.WriteFile(filepath.Join(stage, ".ignore"), []byte(""), 0640); err != nil {
		log.Fatal(err)
	}
	store, err := openStore(filepath.Join(state, "store.json"))
	if err != nil {
		log.Fatal(err)
	}
	s := &Server{store: store, media: media, stage: stage, uploadLocks: make(map[string]*sync.Mutex)}
	for _, cidr := range strings.Split(env("MB_ADMIN_ALLOWED_CIDRS", "127.0.0.1/32,::1/128"), ",") {
		_, netw, err := net.ParseCIDR(strings.TrimSpace(cidr))
		if err != nil {
			log.Fatalf("invalid admin CIDR %q: %v", cidr, err)
		}
		s.allowed = append(s.allowed, netw)
	}
	admin := http.NewServeMux()
	admin.HandleFunc("GET /", s.adminPage)
	admin.HandleFunc("GET /app.js", s.adminAsset)
	admin.HandleFunc("GET /app.css", s.adminAsset)
	admin.HandleFunc("GET /admin/api/state", s.adminState)
	admin.HandleFunc("POST /admin/api/approve/{id}", s.approve)
	admin.HandleFunc("POST /admin/api/revoke/{id}", s.revoke)
	admin.HandleFunc("POST /admin/api/tailscale/login", s.tailscaleLogin)
	admin.HandleFunc("POST /admin/api/tailscale/logout", s.tailscaleLogout)
	data := http.NewServeMux()
	data.HandleFunc("GET /api/v1/health", func(w http.ResponseWriter, r *http.Request) { respond(w, 200, map[string]string{"status": "ok"}) })
	data.HandleFunc("POST /api/v1/pair", s.pair)
	data.HandleFunc("GET /api/v1/pair/status", s.pairStatus)
	data.HandleFunc("POST /api/v1/uploads", s.startUpload)
	data.HandleFunc("PUT /api/v1/uploads/{id}/chunk", s.chunk)
	data.HandleFunc("POST /api/v1/uploads/{id}/finalize", s.finalize)
	adminSrv := &http.Server{Addr: env("MB_ADMIN_LISTEN", ":8080"), Handler: s.guardAdmin(admin), ReadHeaderTimeout: 5 * time.Second}
	dataSrv := &http.Server{Addr: env("MB_DATA_LISTEN", ":8081"), Handler: data, ReadHeaderTimeout: 5 * time.Second}
	go func() {
		log.Printf("management listening on %s", adminSrv.Addr)
		if err := adminSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatal(err)
		}
	}()
	go s.ensureTailnetForward()
	log.Printf("Tailnet receiver listening on %s", dataSrv.Addr)
	log.Fatal(dataSrv.ListenAndServe())
}

// Configure raw TCP forwarding only after the node has joined its tailnet.
// It uses a Docker-network service name, so the HTTP receiver has no host-port
// publication and remains reachable only through Tailscale's identity policy.
func (s *Server) ensureTailnetForward() {
	ticker := time.NewTicker(12 * time.Second)
	defer ticker.Stop()
	for range ticker.C {
		ctx, cancel := context.WithTimeout(context.Background(), 9*time.Second)
		status := exec.CommandContext(ctx, "tailscale", "--socket="+env("MB_TS_SOCKET", "/run/tailscale/tailscaled.sock"), "status", "--json")
		b, err := status.Output()
		cancel()
		var state struct {
			BackendState string `json:"BackendState"`
		}
		if err != nil || json.Unmarshal(b, &state) != nil || state.BackendState != "Running" {
			continue
		}
		ctx, cancel = context.WithTimeout(context.Background(), 9*time.Second)
		cmd := exec.CommandContext(ctx, "tailscale", "--socket="+env("MB_TS_SOCKET", "/run/tailscale/tailscaled.sock"), "serve", "--bg", "--tcp=8081", "tcp://mediabridge:8081")
		out, err := cmd.CombinedOutput()
		cancel()
		if err != nil {
			log.Printf("unable to configure Tailnet receiver forward: %v %s", err, strings.TrimSpace(string(out)))
			continue
		}
		log.Printf("Tailnet receiver forwarding configured on TCP 8081")
		return
	}
}

func respond(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
func fail(w http.ResponseWriter, status int, why string) {
	respond(w, status, map[string]string{"error": why})
}
func decode(r *http.Request, v any) error {
	if !strings.HasPrefix(r.Header.Get("Content-Type"), "application/json") {
		return errors.New("expected application/json")
	}
	return json.NewDecoder(io.LimitReader(r.Body, 64<<10)).Decode(v)
}

func (s *Server) guardAdmin(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		host, _, err := net.SplitHostPort(r.RemoteAddr)
		if err != nil {
			http.Error(w, "forbidden", 403)
			return
		}
		ip := net.ParseIP(host)
		ok := false
		for _, n := range s.allowed {
			if n.Contains(ip) {
				ok = true
				break
			}
		}
		if !ok {
			http.Error(w, "management available through the approved proxy only", 403)
			return
		}
		if r.Method == "POST" {
			origin := r.Header.Get("Origin")
			if origin != "" && origin != "https://mbridge.evalisk.org" {
				http.Error(w, "bad origin", 403)
				return
			}
			if !strings.HasPrefix(r.Header.Get("Content-Type"), "application/json") {
				http.Error(w, "JSON required", 415)
				return
			}
		}
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; base-uri 'none'; frame-ancestors 'none'")
		next.ServeHTTP(w, r)
	})
}

func (s *Server) adminPage(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	b, err := webAssets.ReadFile("web/index.html")
	if err != nil {
		http.Error(w, "missing UI", 500)
		return
	}
	_, _ = w.Write(b)
}
func (s *Server) adminAsset(w http.ResponseWriter, r *http.Request) {
	name := strings.TrimPrefix(r.URL.Path, "/")
	if name != "app.js" && name != "app.css" {
		http.NotFound(w, r)
		return
	}
	b, err := webAssets.ReadFile("web/" + name)
	if err != nil {
		http.Error(w, "missing asset", 500)
		return
	}
	if name == "app.js" {
		w.Header().Set("Content-Type", "text/javascript; charset=utf-8")
	} else {
		w.Header().Set("Content-Type", "text/css; charset=utf-8")
	}
	_, _ = w.Write(b)
}

func authHeader(r *http.Request) string {
	parts := strings.SplitN(r.Header.Get("Authorization"), " ", 2)
	if len(parts) != 2 || !strings.EqualFold(parts[0], "Bearer") {
		return ""
	}
	return parts[1]
}

var hexID = regexp.MustCompile(`^[a-f0-9]{32}$`)
var tokenRE = regexp.MustCompile(`^[a-f0-9]{64}$`)

func (s *Server) identify(r *http.Request, approved bool) (string, bool) {
	id := r.Header.Get("X-MediaBridge-Device")
	token := authHeader(r)
	if !hexID.MatchString(id) || !tokenRE.MatchString(token) {
		return "", false
	}
	s.store.mu.Lock()
	defer s.store.mu.Unlock()
	d := s.store.data.Devices[id]
	if d == nil || subtle.ConstantTimeCompare([]byte(d.TokenHash), []byte(hashToken(token))) != 1 {
		return "", false
	}
	if approved && d.State != "approved" {
		return "", false
	}
	return id, true
}

func (s *Server) pair(w http.ResponseWriter, r *http.Request) {
	var in struct {
		ID    string `json:"id"`
		Name  string `json:"name"`
		Token string `json:"token"`
	}
	if err := decode(r, &in); err != nil {
		fail(w, 400, err.Error())
		return
	}
	in.Name = strings.TrimSpace(in.Name)
	if !hexID.MatchString(in.ID) || !tokenRE.MatchString(in.Token) || len(in.Name) == 0 || len(in.Name) > 80 {
		fail(w, 400, "invalid device identity")
		return
	}
	s.store.mu.Lock()
	defer s.store.mu.Unlock()
	d := s.store.data.Devices[in.ID]
	if d != nil {
		if subtle.ConstantTimeCompare([]byte(d.TokenHash), []byte(hashToken(in.Token))) != 1 {
			fail(w, 409, "device ID collision")
			return
		}
		respond(w, 200, map[string]string{"state": d.State})
		return
	}
	s.store.data.Devices[in.ID] = &Device{ID: in.ID, Name: in.Name, TokenHash: hashToken(in.Token), State: "pending", Created: time.Now().UTC()}
	if err := s.store.save(); err != nil {
		delete(s.store.data.Devices, in.ID)
		fail(w, 500, "unable to persist request")
		return
	}
	respond(w, 201, map[string]string{"state": "pending"})
}

func (s *Server) pairStatus(w http.ResponseWriter, r *http.Request) {
	id, ok := s.identify(r, false)
	if !ok {
		fail(w, 401, "unknown device")
		return
	}
	s.store.mu.Lock()
	d := s.store.data.Devices[id]
	state := d.State
	s.store.mu.Unlock()
	respond(w, 200, map[string]string{"state": state})
}

type publicDevice struct {
	ID       string    `json:"id"`
	Name     string    `json:"name"`
	State    string    `json:"state"`
	Created  time.Time `json:"created"`
	Approved time.Time `json:"approved,omitempty"`
}

func (s *Server) adminState(w http.ResponseWriter, r *http.Request) {
	s.store.mu.Lock()
	devices := []publicDevice{}
	for _, d := range s.store.data.Devices {
		devices = append(devices, publicDevice{d.ID, d.Name, d.State, d.Created, d.Approved})
	}
	active := 0
	for _, u := range s.store.data.Uploads {
		if !u.Complete {
			active++
		}
	}
	s.store.mu.Unlock()
	ctx, cancel := context.WithTimeout(r.Context(), 4*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "tailscale", "--socket="+env("MB_TS_SOCKET", "/run/tailscale/tailscaled.sock"), "status", "--json")
	b, err := cmd.Output()
	ts := map[string]any{"state": "unavailable"}
	if err == nil {
		var x struct {
			BackendState string `json:"BackendState"`
			Self         struct {
				TailscaleIPs []string `json:"TailscaleIPs"`
				HostName     string   `json:"HostName"`
			} `json:"Self"`
		}
		if json.Unmarshal(b, &x) == nil {
			ts = map[string]any{"state": x.BackendState, "ips": x.Self.TailscaleIPs, "hostname": x.Self.HostName}
		}
	}
	s.loginMu.Lock()
	url := s.loginURL
	busy := s.loginBusy
	s.loginMu.Unlock()
	respond(w, 200, map[string]any{"devices": devices, "tailscale": ts, "login_url": url, "login_busy": busy, "active_uploads": active})
}

func (s *Server) approve(w http.ResponseWriter, r *http.Request) { s.changeDevice(w, r, "approved") }
func (s *Server) revoke(w http.ResponseWriter, r *http.Request)  { s.changeDevice(w, r, "revoked") }
func (s *Server) changeDevice(w http.ResponseWriter, r *http.Request, state string) {
	id := r.PathValue("id")
	if !hexID.MatchString(id) {
		fail(w, 400, "invalid id")
		return
	}
	s.store.mu.Lock()
	defer s.store.mu.Unlock()
	d := s.store.data.Devices[id]
	if d == nil {
		fail(w, 404, "device not found")
		return
	}
	if d.State == "revoked" && state == "approved" {
		fail(w, 409, "revoked devices cannot be reapproved; pair as a new device")
		return
	}
	old := *d
	d.State = state
	if state == "approved" {
		d.Approved = time.Now().UTC()
	}
	if err := s.store.save(); err != nil {
		*d = old
		fail(w, 500, "state save failed")
		return
	}
	respond(w, 200, map[string]string{"state": state})
}

var loginURLPattern = regexp.MustCompile(`https://login\.tailscale\.com/[a-zA-Z0-9/?=_-]+`)

func (s *Server) tailscaleLogin(w http.ResponseWriter, r *http.Request) {
	s.loginMu.Lock()
	if s.loginBusy {
		s.loginMu.Unlock()
		fail(w, 409, "login already in progress")
		return
	}
	s.loginBusy = true
	s.loginURL = ""
	s.loginMu.Unlock()
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 4*time.Minute)
		defer cancel()
		cmd := exec.CommandContext(ctx, "tailscale", "--socket="+env("MB_TS_SOCKET", "/run/tailscale/tailscaled.sock"), "login", "--timeout=3m")
		pipe, err := cmd.StdoutPipe()
		if err != nil {
			log.Printf("tailscale login pipe: %v", err)
			s.finishLogin()
			return
		}
		cmd.Stderr = cmd.Stdout
		if err := cmd.Start(); err != nil {
			log.Printf("tailscale login start: %v", err)
			s.finishLogin()
			return
		}
		b := make([]byte, 512)
		var collected string
		for {
			n, err := pipe.Read(b)
			if n > 0 {
				collected += string(b[:n])
				if len(collected) > 8192 {
					collected = collected[len(collected)-8192:]
				}
				u := loginURLPattern.FindString(collected)
				if u != "" {
					s.loginMu.Lock()
					s.loginURL = u
					s.loginMu.Unlock()
				}
			}
			if err != nil {
				break
			}
		}
		if err := cmd.Wait(); err != nil {
			log.Printf("tailscale login ended: %v", err)
		}
		s.finishLogin()
	}()
	respond(w, 202, map[string]string{"state": "starting"})
}
func (s *Server) finishLogin() { s.loginMu.Lock(); s.loginBusy = false; s.loginMu.Unlock() }
func (s *Server) tailscaleLogout(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), 12*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "tailscale", "--socket="+env("MB_TS_SOCKET", "/run/tailscale/tailscaled.sock"), "logout")
	if b, err := cmd.CombinedOutput(); err != nil {
		fail(w, 500, fmt.Sprintf("logout failed: %s", strings.TrimSpace(string(b))))
		return
	}
	s.loginMu.Lock()
	s.loginURL = ""
	s.loginMu.Unlock()
	respond(w, 200, map[string]string{"state": "logged_out"})
}

func paramInt(r *http.Request, key string) (int64, error) {
	return strconv.ParseInt(r.URL.Query().Get(key), 10, 64)
}

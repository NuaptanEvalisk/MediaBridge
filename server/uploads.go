// SPDX-License-Identifier: GPL-3.0-only
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
)

func validPath(name string) bool {
	if len(name) == 0 || len(name) > 4096 || strings.HasPrefix(name, "/") || strings.ContainsAny(name, "\\\x00") {
		return false
	}
	for _, part := range strings.Split(name, "/") {
		if part == "" || part == "." || part == ".." || strings.HasPrefix(part, ".mediabridge-") {
			return false
		}
	}
	return true
}

func (s *Server) startUpload(w http.ResponseWriter, r *http.Request) {
	owner, ok := s.identify(r, true)
	if !ok {
		fail(w, 403, "device is not approved")
		return
	}
	var in struct {
		Path    string `json:"path"`
		Size    int64  `json:"size"`
		ModTime int64  `json:"mod_time"`
	}
	if err := decode(r, &in); err != nil {
		fail(w, 400, err.Error())
		return
	}
	if !validPath(in.Path) || in.Size < 0 || in.Size > (1<<50) {
		fail(w, 400, "invalid path or size")
		return
	}
	dest := filepath.Join(s.media, filepath.FromSlash(in.Path))
	if _, err := os.Lstat(dest); err == nil {
		fail(w, 409, "destination exists; files are never overwritten")
		return
	} else if !errors.Is(err, os.ErrNotExist) {
		fail(w, 500, "cannot inspect destination")
		return
	}
	s.store.mu.Lock()
	defer s.store.mu.Unlock()
	for _, u := range s.store.data.Uploads {
		if u.Owner == owner && u.Path == in.Path && u.Size == in.Size && u.ModTime == in.ModTime && !u.Complete {
			info, err := os.Stat(filepath.Join(s.stage, u.ID+".part"))
			if err == nil {
				respond(w, 200, map[string]any{"id": u.ID, "offset": info.Size()})
				return
			}
		}
	}
	id, err := randomID(16)
	if err != nil {
		fail(w, 500, "randomness unavailable")
		return
	}
	f, err := os.OpenFile(filepath.Join(s.stage, id+".part"), os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0660)
	if err != nil {
		fail(w, 500, "unable to initialize staging")
		return
	}
	_ = f.Close()
	u := &Upload{ID: id, Owner: owner, Path: in.Path, Size: in.Size, ModTime: in.ModTime, Created: time.Now().UTC()}
	s.store.data.Uploads[id] = u
	if err := s.store.save(); err != nil {
		delete(s.store.data.Uploads, id)
		_ = os.Remove(filepath.Join(s.stage, id+".part"))
		fail(w, 500, "unable to persist upload")
		return
	}
	respond(w, 201, map[string]any{"id": id, "offset": int64(0)})
}

func (s *Server) ownedUpload(r *http.Request) (*Upload, bool) {
	owner, ok := s.identify(r, true)
	if !ok {
		return nil, false
	}
	id := r.PathValue("id")
	if !hexID.MatchString(id) {
		return nil, false
	}
	s.store.mu.Lock()
	defer s.store.mu.Unlock()
	u := s.store.data.Uploads[id]
	if u == nil || u.Owner != owner || u.Complete {
		return nil, false
	}
	cp := *u
	return &cp, true
}

func (s *Server) uploadLock(id string) *sync.Mutex {
	s.uploadMu.Lock()
	defer s.uploadMu.Unlock()
	if s.uploadLocks == nil {
		s.uploadLocks = make(map[string]*sync.Mutex)
	}
	if mu := s.uploadLocks[id]; mu != nil {
		return mu
	}
	mu := new(sync.Mutex)
	s.uploadLocks[id] = mu
	return mu
}

func (s *Server) chunk(w http.ResponseWriter, r *http.Request) {
	u, ok := s.ownedUpload(r)
	if !ok {
		fail(w, 403, "unapproved or unknown upload")
		return
	}
	offset, err := paramInt(r, "offset")
	if err != nil || offset < 0 {
		fail(w, 400, "invalid offset")
		return
	}
	if r.ContentLength <= 0 || r.ContentLength > chunkLimit || offset+r.ContentLength > u.Size {
		fail(w, 413, "chunk exceeds limit or file size")
		return
	}
	// Serialize only this upload, not all clients' transfers.
	lock := s.uploadLock(u.ID)
	lock.Lock()
	defer lock.Unlock()
	s.store.mu.Lock()
	if current := s.store.data.Uploads[u.ID]; current == nil || current.ChunkSize != 0 {
		s.store.mu.Unlock()
		fail(w, 409, "upload is using parallel chunk protocol")
		return
	}
	s.store.mu.Unlock()
	s.store.mu.Lock()
	d := s.store.data.Devices[u.Owner]
	approved := d != nil && d.State == "approved"
	s.store.mu.Unlock()
	if !approved {
		fail(w, 403, "device revoked")
		return
	}
	part := filepath.Join(s.stage, u.ID+".part")
	f, err := os.OpenFile(part, os.O_RDWR, 0)
	if err != nil {
		fail(w, 500, "staging file missing")
		return
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		fail(w, 500, "staging inaccessible")
		return
	}
	if info.Size() != offset {
		respond(w, 409, map[string]any{"error": "offset mismatch", "offset": info.Size()})
		return
	}
	if _, err := f.Seek(offset, io.SeekStart); err != nil {
		fail(w, 500, "seek failed")
		return
	}
	n, err := io.CopyN(f, r.Body, r.ContentLength)
	if err != nil || n != r.ContentLength {
		_ = f.Truncate(offset)
		fail(w, 400, "incomplete chunk")
		return
	}
	if err := f.Sync(); err != nil {
		fail(w, 500, "unable to sync chunk")
		return
	}
	respond(w, 200, map[string]any{"offset": offset + n})
}

func (s *Server) finalize(w http.ResponseWriter, r *http.Request) {
	u, ok := s.ownedUpload(r)
	if !ok {
		fail(w, 403, "unapproved or unknown upload")
		return
	}
	var in struct {
		SHA256 string `json:"sha256"`
	}
	if err := decode(r, &in); err != nil {
		fail(w, 400, err.Error())
		return
	}
	if len(in.SHA256) != 64 {
		fail(w, 400, "SHA-256 required")
		return
	}
	if _, err := hex.DecodeString(in.SHA256); err != nil {
		fail(w, 400, "invalid SHA-256")
		return
	}
	lock := s.uploadLock(u.ID)
	lock.Lock()
	defer lock.Unlock()
	s.store.mu.Lock()
	d := s.store.data.Devices[u.Owner]
	approved := d != nil && d.State == "approved"
	s.store.mu.Unlock()
	if !approved {
		fail(w, 403, "device revoked")
		return
	}
	part := filepath.Join(s.stage, u.ID+".part")
	f, err := os.Open(part)
	if err != nil {
		fail(w, 500, "staging missing")
		return
	}
	h := sha256.New()
	n, err := io.Copy(h, f)
	_ = f.Close()
	if err != nil || n != u.Size {
		fail(w, 409, "incomplete file")
		return
	}
	if !strings.EqualFold(hex.EncodeToString(h.Sum(nil)), in.SHA256) {
		fail(w, 422, "SHA-256 mismatch")
		return
	}
	dest, err := s.destinationPath(u.Path)
	if err != nil {
		fail(w, 409, err.Error())
		return
	}
	// Recheck authorization at the last possible point before publishing.
	s.store.mu.Lock()
	defer s.store.mu.Unlock()
	d = s.store.data.Devices[u.Owner]
	if d == nil || d.State != "approved" {
		fail(w, 403, "device revoked")
		return
	}
	// A hard-link is an atomic no-clobber publication on the same ZFS dataset.
	// An already existing destination is never replaced.
	if err := ensureMediaFilePermissions(part); err != nil {
		fail(w, 500, fmt.Sprintf("cannot prepare media permissions: %v", err))
		return
	}
	if err := os.Link(part, dest); err != nil {
		if errors.Is(err, os.ErrExist) {
			fail(w, 409, "destination already exists")
			return
		}
		fail(w, 500, fmt.Sprintf("commit failed: %v", err))
		return
	}
	_ = os.Remove(part)
	s.store.data.Uploads[u.ID].Complete = true
	if err := s.store.save(); err != nil {
		log.Printf("completed upload metadata save failed: %v", err)
	}
	respond(w, 200, map[string]any{"state": "complete", "path": u.Path})
}

// A previous commit attempt might already have changed the staged file's
// owner before a later filesystem operation failed. Avoid chmod/chown when
// the requested metadata is already present: repeated operations are not
// harmless under restricted Linux container capabilities or inherited ACLs.
func ensureMediaFilePermissions(path string) error {
	info, err := os.Lstat(path)
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("staging path is not a regular file")
	}
	if info.Mode().Perm() != 0660 {
		if err := os.Chmod(path, 0660); err != nil {
			return fmt.Errorf("chmod: %w", err)
		}
	}
	if os.Geteuid() == 0 {
		stat, ok := info.Sys().(*syscall.Stat_t)
		if !ok {
			return fmt.Errorf("cannot read file ownership")
		}
		if stat.Uid != 33 || stat.Gid != 568 {
			if err := os.Chown(path, 33, 568); err != nil {
				return fmt.Errorf("chown: %w", err)
			}
		}
	}
	return nil
}

func (s *Server) destinationPath(rel string) (string, error) {
	if !validPath(rel) {
		return "", errors.New("invalid path")
	}
	base := s.media
	parts := strings.Split(rel, "/")
	for _, part := range parts[:len(parts)-1] {
		base = filepath.Join(base, part)
		fi, err := os.Lstat(base)
		if errors.Is(err, os.ErrNotExist) {
			if err := os.Mkdir(base, 02770); err != nil {
				return "", err
			}
			if err := os.Chmod(base, 02770); err != nil {
				return "", err
			}
			if os.Geteuid() == 0 {
				if err := os.Chown(base, 33, 568); err != nil {
					return "", err
				}
			}
			continue
		}
		if err != nil {
			return "", err
		}
		if !fi.IsDir() || fi.Mode()&os.ModeSymlink != 0 {
			return "", errors.New("unsafe destination parent")
		}
	}
	dest := filepath.Join(base, parts[len(parts)-1])
	if _, err := os.Lstat(dest); err == nil {
		return "", errors.New("destination already exists")
	} else if !errors.Is(err, os.ErrNotExist) {
		return "", err
	}
	return dest, nil
}

// Keep JSON state compatible with straightforward offline inspection.
var _ = json.Valid

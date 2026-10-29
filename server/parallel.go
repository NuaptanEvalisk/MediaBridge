// SPDX-License-Identifier: GPL-3.0-only
package main

import (
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"time"
)

const parallelChunkSize int64 = 4 << 20

func numberOfChunks(size int64) int {
	return int((size + parallelChunkSize - 1) / parallelChunkSize)
}
func hasBit(b []byte, index int) bool {
	return b[index/8]&(1<<uint(index%8)) != 0
}
func setBit(b []byte, index int) {
	b[index/8] |= 1 << uint(index%8)
}
func allBitsSet(b []byte, count int) bool {
	for i := 0; i < count; i++ {
		if !hasBit(b, i) {
			return false
		}
	}
	return true
}
func parallelResponse(u *Upload) map[string]any {
	return map[string]any{"id": u.ID, "chunk_size": parallelChunkSize, "bitmap": u.Bitmap}
}

// Upgrades an incomplete V1 upload without discarding fully written chunks.
// Persist the bitmap before file resizing so interrupted migrations are safe.
func (s *Server) startParallelUpload(w http.ResponseWriter, r *http.Request) {
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
		if u.Owner != owner || u.Path != in.Path || u.Size != in.Size || u.ModTime != in.ModTime || u.Complete {
			continue
		}
		part := filepath.Join(s.stage, u.ID+".part")
		stat, err := os.Stat(part)
		if err != nil {
			continue
		}
		if u.ChunkSize == 0 {
			if stat.Size() > u.Size {
				fail(w, 500, "invalid legacy staging size")
				return
			}
			oldSize := stat.Size()
			u.ChunkSize = parallelChunkSize
			u.Bitmap = make([]byte, (numberOfChunks(u.Size)+7)/8)
			for i := 0; i < numberOfChunks(u.Size); i++ {
				end := int64(i+1) * parallelChunkSize
				if end > u.Size {
					end = u.Size
				}
				if end <= oldSize {
					setBit(u.Bitmap, i)
				}
			}
			if err := s.store.save(); err != nil {
				fail(w, 500, "could not persist migration")
				return
			}
		}
		if u.ChunkSize != parallelChunkSize {
			fail(w, 409, "unsupported chunk size")
			return
		}
		if stat.Size() != u.Size {
			if err := os.Truncate(part, u.Size); err != nil {
				fail(w, 500, "cannot size staging file")
				return
			}
		}
		respond(w, 200, parallelResponse(u))
		return
	}
	id, err := randomID(16)
	if err != nil {
		fail(w, 500, "randomness unavailable")
		return
	}
	part := filepath.Join(s.stage, id+".part")
	f, err := os.OpenFile(part, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0660)
	if err != nil {
		fail(w, 500, "cannot create staging file")
		return
	}
	if err := f.Truncate(in.Size); err != nil {
		f.Close()
		os.Remove(part)
		fail(w, 500, "cannot size staging file")
		return
	}
	if err := f.Close(); err != nil {
		os.Remove(part)
		fail(w, 500, "cannot close staging file")
		return
	}
	u := &Upload{ID: id, Owner: owner, Path: in.Path, Size: in.Size, ModTime: in.ModTime, Created: time.Now().UTC(),
		ChunkSize: parallelChunkSize, Bitmap: make([]byte, (numberOfChunks(in.Size)+7)/8)}
	s.store.data.Uploads[id] = u
	if err := s.store.save(); err != nil {
		delete(s.store.data.Uploads, id)
		os.Remove(part)
		fail(w, 500, "cannot persist upload")
		return
	}
	respond(w, 201, parallelResponse(u))
}

func (s *Server) parallelChunk(w http.ResponseWriter, r *http.Request) {
	u, ok := s.ownedUpload(r)
	if !ok {
		fail(w, 403, "unapproved or unknown upload")
		return
	}
	index, err := strconv.Atoi(r.PathValue("index"))
	count := numberOfChunks(u.Size)
	if err != nil || index < 0 || index >= count {
		fail(w, 400, "invalid chunk index")
		return
	}
	offset := int64(index) * parallelChunkSize
	expected := parallelChunkSize
	if offset+expected > u.Size {
		expected = u.Size - offset
	}
	if r.ContentLength != expected {
		fail(w, 400, "unexpected chunk length")
		return
	}

	// Distinct chunk indices write concurrently; duplicates are retried.
	s.uploadMu.Lock()
	if s.inFlight == nil {
		s.inFlight = make(map[string]map[int]bool)
	}
	if s.inFlight[u.ID] == nil {
		s.inFlight[u.ID] = make(map[int]bool)
	}
	if s.inFlight[u.ID][index] {
		s.uploadMu.Unlock()
		fail(w, 409, "chunk already in progress")
		return
	}
	s.inFlight[u.ID][index] = true
	s.uploadMu.Unlock()
	defer func() { s.uploadMu.Lock(); delete(s.inFlight[u.ID], index); s.uploadMu.Unlock() }()

	s.store.mu.Lock()
	current := s.store.data.Uploads[u.ID]
	d := s.store.data.Devices[u.Owner]
	approved := d != nil && d.State == "approved"
	if current == nil || current.Complete || current.ChunkSize != parallelChunkSize || !approved {
		s.store.mu.Unlock()
		fail(w, 403, "upload not permitted")
		return
	}
	if hasBit(current.Bitmap, index) {
		s.store.mu.Unlock()
		respond(w, 200, map[string]any{"index": index, "already_done": true})
		return
	}
	s.store.mu.Unlock()
	part := filepath.Join(s.stage, u.ID+".part")
	f, err := os.OpenFile(part, os.O_WRONLY, 0)
	if err != nil {
		fail(w, 500, "staging file missing")
		return
	}
	buffer := make([]byte, expected)
	if _, err = io.ReadFull(r.Body, buffer); err != nil {
		f.Close()
		fail(w, 400, "incomplete chunk body")
		return
	}
	n, err := f.WriteAt(buffer, offset)
	if err != nil || int64(n) != expected {
		f.Close()
		fail(w, 500, "failed to write chunk")
		return
	}
	if err = f.Sync(); err != nil {
		f.Close()
		fail(w, 500, "failed to sync chunk")
		return
	}
	if err = f.Close(); err != nil {
		fail(w, 500, "failed to close chunk")
		return
	}
	s.store.mu.Lock()
	defer s.store.mu.Unlock()
	current = s.store.data.Uploads[u.ID]
	d = s.store.data.Devices[u.Owner]
	if current == nil || current.Complete || d == nil || d.State != "approved" {
		fail(w, 403, "device revoked")
		return
	}
	setBit(current.Bitmap, index)
	if err = s.store.save(); err != nil {
		current.Bitmap[index/8] &^= 1 << uint(index%8)
		fail(w, 500, fmt.Sprintf("failed to save chunk progress: %v", err))
		return
	}
	respond(w, 200, map[string]any{"index": index, "complete": true})
}

func (s *Server) finalizeParallel(w http.ResponseWriter, r *http.Request) {
	owner, ok := s.identify(r, true)
	if !ok {
		fail(w, 403, "device is not approved")
		return
	}
	id := r.PathValue("id")
	if !hexID.MatchString(id) {
		fail(w, 400, "invalid upload ID")
		return
	}
	s.store.mu.Lock()
	u := s.store.data.Uploads[id]
	if u == nil || u.Owner != owner {
		s.store.mu.Unlock()
		fail(w, 404, "unknown upload")
		return
	}
	if u.Complete {
		s.store.mu.Unlock()
		respond(w, 200, map[string]string{"state": "complete"})
		return
	}
	if u.ChunkSize != parallelChunkSize || !allBitsSet(u.Bitmap, numberOfChunks(u.Size)) {
		s.store.mu.Unlock()
		fail(w, 409, "incomplete upload")
		return
	}
	s.store.mu.Unlock()
	s.finalize(w, r)
}

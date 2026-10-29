// SPDX-License-Identifier: GPL-3.0-only
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"testing"
)

const testID = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
const testToken = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

func approveForTest(t *testing.T, s *Server) {
	t.Helper()
	s.store.data.Devices[testID] = &Device{ID: testID, Name: "Chelonia", TokenHash: hashToken(testToken), State: "approved"}
	if err := s.store.save(); err != nil {
		t.Fatal(err)
	}
}

func apiTest(t *testing.T, h http.HandlerFunc, method, path string, body []byte, uploadID, chunk string) *httptest.ResponseRecorder {
	t.Helper()
	r := httptest.NewRequest(method, path, bytes.NewReader(body))
	r.Header.Set("X-MediaBridge-Device", testID)
	r.Header.Set("Authorization", "Bearer "+testToken)
	if method == "PUT" {
		r.Header.Set("Content-Type", "application/octet-stream")
	} else {
		r.Header.Set("Content-Type", "application/json")
	}
	if uploadID != "" {
		r.SetPathValue("id", uploadID)
	}
	if chunk != "" {
		r.SetPathValue("index", chunk)
	}
	w := httptest.NewRecorder()
	h(w, r)
	return w
}

func parallelStart(t *testing.T, s *Server, path string, contents []byte) (string, []byte) {
	t.Helper()
	request, _ := json.Marshal(map[string]any{"path": path, "size": len(contents), "mod_time": 100})
	w := apiTest(t, s.startParallelUpload, "POST", "/api/v2/uploads", request, "", "")
	if w.Code != 201 && w.Code != 200 {
		t.Fatalf("start: %d %s", w.Code, w.Body.String())
	}
	var response struct {
		ID     string `json:"id"`
		Bitmap []byte `json:"bitmap"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	return response.ID, response.Bitmap
}
func putParallel(t *testing.T, s *Server, id string, index int, body []byte) {
	t.Helper()
	w := apiTest(t, s.parallelChunk, "PUT", "/api/v2/uploads/x/chunks/y", body, id, itoa(index))
	if w.Code != 200 {
		t.Errorf("chunk %d: %d %s", index, w.Code, w.Body.String())
	}
}
func itoa(n int) string { return strconv.Itoa(n) }

func TestParallelChunksAndIdempotentFinalize(t *testing.T) {
	s := setup(t)
	approveForTest(t, s)
	data := bytes.Repeat([]byte("01234567"), int((parallelChunkSize*2+17)/8+1))
	data = data[:parallelChunkSize*2+17]
	id, bitmap := parallelStart(t, s, "Movies/Test/large.mkv", data)
	if len(bitmap) != 1 {
		t.Fatalf("unexpected bitmap: %v", bitmap)
	}
	// Out-of-order chunks from independent connections, plus duplicate replay.
	var wg sync.WaitGroup
	for _, index := range []int{2, 0, 1} {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			begin := int64(i) * parallelChunkSize
			end := begin + parallelChunkSize
			if end > int64(len(data)) {
				end = int64(len(data))
			}
			putParallel(t, s, id, i, data[int(begin):int(end)])
		}(index)
	}
	wg.Wait()
	putParallel(t, s, id, 0, data[:parallelChunkSize])
	_, bitmap = parallelStart(t, s, "Movies/Test/large.mkv", data)
	if bitmap[0] != 7 {
		t.Fatalf("lost progress: bitmap=%08b", bitmap[0])
	}
	sum := sha256.Sum256(data)
	request, _ := json.Marshal(map[string]string{"sha256": hex.EncodeToString(sum[:])})
	for i := 0; i < 2; i++ {
		w := apiTest(t, s.finalizeParallel, "POST", "/api/v2/uploads/x/finalize", request, id, "")
		if w.Code != 200 {
			t.Fatalf("finalize attempt %d: %d %s", i, w.Code, w.Body.String())
		}
	}
	stored, err := os.ReadFile(filepath.Join(s.media, "Movies/Test/large.mkv"))
	if err != nil || !bytes.Equal(stored, data) {
		t.Fatalf("corrupt uploaded file: %v", err)
	}
}

func TestMigrateLegacyPartialUpload(t *testing.T) {
	s := setup(t)
	approveForTest(t, s)
	data := bytes.Repeat([]byte("q"), int(parallelChunkSize*2+99))
	id := "dddddddddddddddddddddddddddddddd"
	u := &Upload{ID: id, Owner: testID, Path: "Resumed/video.mkv", Size: int64(len(data)), ModTime: 100}
	s.store.data.Uploads[id] = u
	if err := s.store.save(); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(s.stage, id+".part"), data[:parallelChunkSize+19], 0600); err != nil {
		t.Fatal(err)
	}
	resumed, bitmap := parallelStart(t, s, u.Path, data)
	if resumed != id || bitmap[0] != 1 {
		t.Fatalf("migration failed: %s %v", resumed, bitmap)
	}
	putParallel(t, s, id, 1, data[parallelChunkSize:parallelChunkSize*2])
	putParallel(t, s, id, 2, data[parallelChunkSize*2:])
	sum := sha256.Sum256(data)
	req, _ := json.Marshal(map[string]string{"sha256": hex.EncodeToString(sum[:])})
	w := apiTest(t, s.finalizeParallel, "POST", "/api/v2/uploads/x/finalize", req, id, "")
	if w.Code != 200 {
		t.Fatalf("migrated finalize: %d %s", w.Code, w.Body.String())
	}
	stored, err := os.ReadFile(filepath.Join(s.media, u.Path))
	if err != nil || !bytes.Equal(stored, data) {
		t.Fatalf("migrated file corrupt: %v", err)
	}
}

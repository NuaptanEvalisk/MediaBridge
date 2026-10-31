// SPDX-License-Identifier: GPL-3.0-only
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"testing"
	"time"
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

func TestStalledChunkReleasesInFlightSlot(t *testing.T) {
	s := setup(t)
	approveForTest(t, s)
	data := bytes.Repeat([]byte("a"), int(parallelChunkSize))
	id, _ := parallelStart(t, s, "Movies/Interrupted.mkv", data)
	check := func() (bool, bool) {
		t.Helper()
		response := apiTest(t, s.parallelChunkStatus, "GET",
			"/api/v2/uploads/x/chunks/0", nil, id, "0")
		if response.Code != 200 {
			t.Fatalf("status: %d %s", response.Code, response.Body.String())
		}
		var result struct {
			Done       bool `json:"done"`
			InProgress bool `json:"in_progress"`
		}
		if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
			t.Fatal(err)
		}
		return result.Done, result.InProgress
	}
	if done, inProgress := check(); done || inProgress {
		t.Fatal("new chunk incorrectly reported in progress")
	}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		r.SetPathValue("id", id)
		r.SetPathValue("index", "0")
		s.parallelChunkWithIdleTimeout(w, r, 150*time.Millisecond)
	}))
	defer server.Close()
	conn, err := net.Dial("tcp", server.Listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	_, err = fmt.Fprintf(conn, "PUT / HTTP/1.1\r\nHost: localhost\r\n"+
		"X-MediaBridge-Device: %s\r\nAuthorization: Bearer %s\r\n"+
		"Content-Length: %d\r\nContent-Type: application/octet-stream\r\n"+
		"Connection: close\r\n\r\n", testID, testToken, parallelChunkSize)
	if err != nil {
		t.Fatal(err)
	}
	// Send only the headers, deliberately leaving the backend waiting for
	// bytes which will never arrive, as with a broken Tailscale TCP relay.
	deadline := time.Now().Add(2 * time.Second)
	seen := false
	for time.Now().Before(deadline) {
		if _, inProgress := check(); inProgress {
			seen = true
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	if !seen {
		t.Fatal("stalled request never acquired its chunk slot")
	}
	for time.Now().Before(deadline) {
		if done, inProgress := check(); !done && !inProgress {
			putParallel(t, s, id, 0, data)
			if done, inProgress := check(); !done || inProgress {
				t.Fatal("recovered chunk should be completed without a stale lock")
			}
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("stalled chunk retained its slot after the idle deadline")
}

type endWithBytesReader struct {
	data []byte
}

func (r *endWithBytesReader) Read(p []byte) (int, error) {
	if len(r.data) == 0 {
		return 0, io.EOF
	}
	n := copy(p, r.data)
	r.data = r.data[n:]
	return n, io.EOF
}

func TestChunkReadAcceptsFinalBytesWithEOF(t *testing.T) {
	for _, tc := range []struct {
		name      string
		data      string
		size      int
		wantBytes int
		wantError bool
	}{
		{"complete", "abcd", 4, 4, false},
		{"incomplete", "abc", 4, 3, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			r := httptest.NewRequest("PUT", "/chunk", nil)
			r.Body = io.NopCloser(&endWithBytesReader{data: []byte(tc.data)})
			buffer := make([]byte, tc.size)
			got, err := readChunkWithIdleDeadline(httptest.NewRecorder(), r, buffer, time.Second)
			if got != tc.wantBytes || (err != nil) != tc.wantError {
				t.Fatalf("received %d, error %v; expected %d, err=%v",
					got, err, tc.wantBytes, tc.wantError)
			}
			if !tc.wantError && string(buffer) != tc.data {
				t.Fatalf("data corrupted: %q", buffer)
			}
		})
	}
}

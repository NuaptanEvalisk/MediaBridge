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
	"testing"
)

func setup(t *testing.T) *Server {
	t.Helper()
	root := t.TempDir()
	media := filepath.Join(root, "media")
	stage := filepath.Join(media, ".mediabridge-staging")
	if err := os.MkdirAll(stage, 0770); err != nil {
		t.Fatal(err)
	}
	s, err := openStore(filepath.Join(root, "store.json"))
	if err != nil {
		t.Fatal(err)
	}
	return &Server{store: s, media: media, stage: stage}
}

func testRequest(t *testing.T, h http.HandlerFunc, method, path string, body []byte, id, token string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(method, path, bytes.NewReader(body))
	if len(body) > 0 {
		if method == "PUT" {
			req.Header.Set("Content-Type", "application/octet-stream")
		} else {
			req.Header.Set("Content-Type", "application/json")
		}
	}
	if id != "" {
		req.Header.Set("X-MediaBridge-Device", id)
		req.Header.Set("Authorization", "Bearer "+token)
	}
	rec := httptest.NewRecorder()
	h(rec, req)
	return rec
}

func TestApprovalUploadRevoke(t *testing.T) {
	s := setup(t)
	id := "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	token := "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	pair, _ := json.Marshal(map[string]string{"id": id, "name": "Chelonia", "token": token})
	rec := testRequest(t, s.pair, "POST", "/api/v1/pair", pair, "", "")
	if rec.Code != 201 {
		t.Fatalf("pair returned %d: %s", rec.Code, rec.Body.String())
	}
	body, _ := json.Marshal(map[string]any{"path": "Movies/Test/video.mkv", "size": 11, "mod_time": 123})
	if rec = testRequest(t, s.startUpload, "POST", "/api/v1/uploads", body, id, token); rec.Code != 403 {
		t.Fatalf("unapproved write %d", rec.Code)
	}
	request := httptest.NewRequest("POST", "/admin/api/approve/"+id, nil)
	request.SetPathValue("id", id)
	approve := httptest.NewRecorder()
	s.approve(approve, request)
	if approve.Code != 200 {
		t.Fatalf("approve: %s", approve.Body.String())
	}
	rec = testRequest(t, s.startUpload, "POST", "/api/v1/uploads", body, id, token)
	if rec.Code != 201 {
		t.Fatalf("start: %s", rec.Body.String())
	}
	var started struct {
		ID string `json:"id"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &started); err != nil {
		t.Fatal(err)
	}
	req := httptest.NewRequest("PUT", "/api/v1/uploads/"+started.ID+"/chunk?offset=0", bytes.NewBufferString("hello world"))
	req.SetPathValue("id", started.ID)
	req.Header.Set("X-MediaBridge-Device", id)
	req.Header.Set("Authorization", "Bearer "+token)
	rec = httptest.NewRecorder()
	s.chunk(rec, req)
	if rec.Code != 200 {
		t.Fatalf("chunk: %s", rec.Body.String())
	}
	sum := sha256.Sum256([]byte("hello world"))
	payload, _ := json.Marshal(map[string]string{"sha256": hex.EncodeToString(sum[:])})
	req = httptest.NewRequest("POST", "/api/v1/uploads/"+started.ID+"/finalize", bytes.NewReader(payload))
	req.SetPathValue("id", started.ID)
	req.Header.Set("X-MediaBridge-Device", id)
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")
	rec = httptest.NewRecorder()
	s.finalize(rec, req)
	if rec.Code != 200 {
		t.Fatalf("finalize: %s", rec.Body.String())
	}
	saved, err := os.ReadFile(filepath.Join(s.media, "Movies/Test/video.mkv"))
	if err != nil || string(saved) != "hello world" {
		t.Fatalf("wrong media file: %v %q", err, saved)
	}
	request = httptest.NewRequest("POST", "/admin/api/revoke/"+id, nil)
	request.SetPathValue("id", id)
	rev := httptest.NewRecorder()
	s.revoke(rev, request)
	if rev.Code != 200 {
		t.Fatalf("revoke failed: %s", rev.Body.String())
	}
	body, _ = json.Marshal(map[string]any{"path": "Movies/second.mkv", "size": 1, "mod_time": 123})
	rec = testRequest(t, s.startUpload, "POST", "/api/v1/uploads", body, id, token)
	if rec.Code != 403 {
		t.Fatalf("revoked client uploaded: %s", rec.Body.String())
	}
}

func TestPathValidation(t *testing.T) {
	for _, name := range []string{"../media", "/etc/passwd", "x/../../passwd", "x//y", "x/./y", "x\\y", ".mediabridge-staging/file", ""} {
		if validPath(name) {
			t.Errorf("unsafe path accepted: %q", name)
		}
	}
	for _, name := range []string{"Movie.mkv", "Series/Season 1/S01E01.mkv", "电影/第一集.mkv"} {
		if !validPath(name) {
			t.Errorf("safe path rejected: %q", name)
		}
	}
}

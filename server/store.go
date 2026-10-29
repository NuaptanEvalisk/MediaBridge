// SPDX-License-Identifier: GPL-3.0-only
package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"time"
)

type Device struct {
	ID        string    `json:"id"`
	Name      string    `json:"name"`
	TokenHash string    `json:"token_hash"`
	State     string    `json:"state"`
	Created   time.Time `json:"created"`
	Approved  time.Time `json:"approved,omitempty"`
	LastSeen  time.Time `json:"last_seen,omitempty"`
}

type Upload struct {
	ID       string    `json:"id"`
	Owner    string    `json:"owner"`
	Path     string    `json:"path"`
	Size     int64     `json:"size"`
	ModTime  int64     `json:"mod_time"`
	Created  time.Time `json:"created"`
	Complete bool      `json:"complete"`
}

type database struct {
	Devices map[string]*Device `json:"devices"`
	Uploads map[string]*Upload `json:"uploads"`
}

type Store struct {
	mu   sync.Mutex
	path string
	data database
}

func openStore(path string) (*Store, error) {
	s := &Store{path: path, data: database{Devices: map[string]*Device{}, Uploads: map[string]*Upload{}}}
	b, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return s, nil
	}
	if err != nil {
		return nil, err
	}
	if err = json.Unmarshal(b, &s.data); err != nil {
		return nil, err
	}
	if s.data.Devices == nil {
		s.data.Devices = map[string]*Device{}
	}
	if s.data.Uploads == nil {
		s.data.Uploads = map[string]*Upload{}
	}
	return s, nil
}

func (s *Store) save() error {
	b, err := json.MarshalIndent(s.data, "", "  ")
	if err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(s.path), ".state-")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if err = f.Chmod(0600); err != nil {
		f.Close()
		return err
	}
	if _, err = f.Write(b); err != nil {
		f.Close()
		return err
	}
	if err = f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	return os.Rename(f.Name(), s.path)
}

func randomID(n int) (string, error) {
	b := make([]byte, n)
	_, err := rand.Read(b)
	return hex.EncodeToString(b), err
}
func hashToken(t string) string { h := sha256.Sum256([]byte(t)); return hex.EncodeToString(h[:]) }

// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"archive/zip"
	"crypto/sha256"
	"fmt"
	"os"
	"path/filepath"
	"testing"
)

func TestPackageExtractionRejectsUnsafeOrUnhashedEntries(t *testing.T) {
	for _, scenario := range []string{"valid", "traversal", "duplicate", "symlink", "unhashed", "tampered"} {
		t.Run(scenario, func(t *testing.T) {
			root := t.TempDir()
			path := filepath.Join(root, "installer.zip")
			file, err := os.Create(path)
			if err != nil {
				t.Fatal(err)
			}
			archive := zip.NewWriter(file)
			add := func(name string, data []byte, mode os.FileMode) {
				header := &zip.FileHeader{Name: name, Method: zip.Deflate}
				header.SetMode(mode)
				out, err := archive.CreateHeader(header)
				if err != nil {
					t.Fatal(err)
				}
				if _, err := out.Write(data); err != nil {
					t.Fatal(err)
				}
			}
			data := []byte("checked executable")
			sum := sha256.Sum256(data)
			name := "tools/helper"
			mode := os.FileMode(0755)
			switch scenario {
			case "traversal":
				name = "../outside"
			case "symlink":
				mode |= os.ModeSymlink
			case "tampered":
				data = []byte("altered executable")
			}
			add(name, data, mode)
			add("SHA256SUMS", []byte(fmt.Sprintf("%x  %s\n", sum, name)), 0644)
			if scenario == "duplicate" {
				add(name, data, mode)
			}
			if scenario == "unhashed" {
				add("extra", []byte("unlisted executable"), 0755)
			}
			if err := archive.Close(); err != nil {
				t.Fatal(err)
			}
			if err := file.Close(); err != nil {
				t.Fatal(err)
			}
			err = extractPackage(path, filepath.Join(root, "package"))
			if (err == nil) != (scenario == "valid") {
				t.Fatalf("unexpected extraction result: %v", err)
			}
			if _, err := os.Stat(filepath.Join(root, "outside")); !os.IsNotExist(err) {
				t.Fatal("ZIP traversal wrote outside the extraction directory")
			}
		})
	}
}

// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"archive/zip"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

func extractPackage(archive, output string) error {
	z, err := zip.OpenReader(archive)
	if err != nil {
		return err
	}
	defer z.Close()
	if err = os.Mkdir(output, 0700); err != nil {
		return err
	}
	seen := make(map[string]bool)
	var total uint64
	for _, file := range z.File {
		if file.Name == "" || !safePath(file.Name) || strings.ContainsAny(file.Name, " \t") || seen[file.Name] || !file.Mode().IsRegular() {
			return fmt.Errorf("unsafe or duplicate ZIP entry: %q", file.Name)
		}
		seen[file.Name] = true
		total += file.UncompressedSize64
		if file.UncompressedSize64 > 256<<20 || total > 1<<30 {
			return fmt.Errorf("ZIP exceeds installer size limits")
		}
	}
	for _, file := range z.File {
		path := filepath.Join(output, file.Name)
		if err = os.MkdirAll(filepath.Dir(path), 0700); err != nil {
			return err
		}
		mode := os.FileMode(0600)
		if file.Mode()&0111 != 0 {
			mode = 0700
		}
		out, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, mode)
		if err != nil {
			return err
		}
		in, err := file.Open()
		if err != nil {
			out.Close()
			return err
		}
		n, copyErr := io.Copy(out, io.LimitReader(in, int64(file.UncompressedSize64)+1))
		in.Close()
		closeErr := out.Close()
		if copyErr != nil || closeErr != nil || uint64(n) != file.UncompressedSize64 {
			return fmt.Errorf("ZIP extraction failed: %s", file.Name)
		}
	}
	manifest, err := os.ReadFile(filepath.Join(output, "SHA256SUMS"))
	if err != nil {
		return err
	}
	checked := map[string]bool{"SHA256SUMS": true}
	for _, line := range strings.Split(strings.TrimSpace(string(manifest)), "\n") {
		fields := strings.Fields(line)
		if len(fields) != 2 || len(fields[0]) != 64 || !seen[fields[1]] || checked[fields[1]] {
			return fmt.Errorf("invalid ZIP checksum inventory")
		}
		digest, err := fileSHA(filepath.Join(output, fields[1]))
		if err != nil || digest != fields[0] {
			return fmt.Errorf("ZIP checksum mismatch: %s", fields[1])
		}
		checked[fields[1]] = true
	}
	if len(checked) != len(seen) {
		return fmt.Errorf("ZIP contains unhashed files")
	}
	return nil
}

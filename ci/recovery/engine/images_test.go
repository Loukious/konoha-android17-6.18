// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"bytes"
	"encoding/hex"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestEROFSMetadataRoundTrip(t *testing.T) {
	tools := os.Getenv("ONYX_TEST_TOOLS")
	if tools == "" {
		t.Skip("set ONYX_TEST_TOOLS to the tested image utilities")
	}
	root := t.TempDir()
	data := []byte("retained payload\x00with binary contents\n")
	payload := filepath.Join(root, "payload")
	if err := os.WriteFile(payload, data, 0600); err != nil {
		t.Fatal(err)
	}
	digest, _ := fileSHA(payload)
	xattr := func(name, value string) (string, string) {
		return hex.EncodeToString([]byte(name)), hex.EncodeToString([]byte(value))
	}
	attrs := make(map[string]string)
	for _, pair := range [][2]string{{"security.selinux", "u:object_r:vendor_file:s0"}, {"user.binary", "\x00\xff\n=\x00"}, {"user.empty", ""}} {
		key, value := xattr(pair[0], pair[1])
		attrs[key] = value
	}
	base := inode{Mode: 0100640, UID: 1001, GID: 3003, NID: 7, Size: uint64(len(data)), Mtime: 1793923200, MtimeNsec: 123450000, Xattrs: attrs}
	tree := &imageTree{FS: filesystem{UUID: "00112233445566778899aabbccddeeff", Label: hex.EncodeToString([]byte("fixture\x00\x00\x00\x00\x00\x00\x00\x00\x00")), BlockSize: 4096}, Entries: make(map[string]*entry)}
	for _, name := range []string{"", "lib", "lib/modules"} {
		meta := base
		meta.NID = uint64(len(tree.Entries) + 1)
		meta.Mode = 0040750
		meta.Size = 0
		meta.PathHex = hex.EncodeToString([]byte(name))
		tree.Entries[name] = &entry{Meta: meta}
	}
	for _, name := range []string{"lib/modules/kept", "lib/modules/hardlink"} {
		meta := base
		meta.PathHex = hex.EncodeToString([]byte(name))
		tree.Entries[name] = &entry{Meta: meta, Source: payload, Digest: digest}
	}
	meta := base
	meta.NID = 8
	meta.Mode = 0120777
	meta.Size = 4
	meta.PathHex = hex.EncodeToString([]byte("lib/modules/link"))
	tree.Entries["lib/modules/link"] = &entry{Meta: meta, Link: "kept"}
	if err := writeEROFS(tools, tree, filepath.Join(root, "image.erofs"), root); err != nil {
		t.Fatal(err)
	}
}

func TestAVBWithIndependentVerifier(t *testing.T) {
	tool := os.Getenv("ONYX_TEST_AVBTOOL")
	if tool == "" {
		t.Skip("set ONYX_TEST_AVBTOOL to Google's avbtool.py")
	}
	for _, item := range []struct {
		kind, algorithm string
		size            int
	}{
		{"hash", "sha256", 12345}, {"hashtree", "sha256", 4096},
		{"hashtree", "sha256", 1048576}, {"hashtree", "sha1", 1048576},
	} {
		t.Run(item.kind+item.algorithm+fmt.Sprint(item.size), func(t *testing.T) {
			root := t.TempDir()
			old := filepath.Join(root, "original.img")
			updated := filepath.Join(root, "vendor_dlkm.img")
			payload := bytes.Repeat([]byte{0x55}, item.size)
			if err := os.WriteFile(old, payload, 0600); err != nil {
				t.Fatal(err)
			}
			args := []string{tool, "add_" + item.kind + "_footer", "--image", old, "--partition_name", "vendor_dlkm",
				"--partition_size", "2097152", "--algorithm", "NONE", "--hash_algorithm", item.algorithm,
				"--salt", "11223344", "--rollback_index", "12345", "--prop", "fixture:preserved"}
			if item.kind == "hashtree" {
				args = append(args, "--do_not_generate_fec")
			}
			if out, err := exec.Command("python3", args...).CombinedOutput(); err != nil {
				t.Fatalf("fixture: %v: %s", err, out)
			}
			payload[42] ^= 0xff
			if err := os.WriteFile(updated, payload, 0600); err != nil {
				t.Fatal(err)
			}
			if err := writeAVB(old, updated, "vendor_dlkm"); err != nil {
				t.Fatal(err)
			}
			if out, err := exec.Command("python3", tool, "verify_image", "--image", updated).CombinedOutput(); err != nil {
				t.Fatalf("independent verification: %v: %s", err, out)
			}
			out, err := exec.Command("python3", tool, "info_image", "--image", updated).CombinedOutput()
			if err != nil || !strings.Contains(string(out), "12345") || !strings.Contains(string(out), "fixture -> 'preserved'") {
				t.Fatalf("metadata not preserved: %v: %s", err, out)
			}
		})
	}
}

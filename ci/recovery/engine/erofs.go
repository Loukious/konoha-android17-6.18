// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"archive/tar"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"time"
	"unicode/utf8"
)

type filesystem struct {
	UUID      string `json:"uuid_hex"`
	Label     string `json:"label_hex"`
	BlockSize int    `json:"block_size"`
}

type inode struct {
	Filesystem *filesystem       `json:"filesystem,omitempty"`
	PathHex    string            `json:"path_hex"`
	NID        uint64            `json:"nid"`
	Mode       uint32            `json:"mode"`
	UID        uint32            `json:"uid"`
	GID        uint32            `json:"gid"`
	Size       uint64            `json:"size"`
	Mtime      int64             `json:"mtime"`
	MtimeNsec  int64             `json:"mtime_nsec"`
	Xattrs     map[string]string `json:"xattrs"`
}

type entry struct {
	Meta   inode
	Source string
	Link   string
	Digest string
}

type imageTree struct {
	FS      filesystem
	Entries map[string]*entry
}

func invoke(tools, name string, args ...string) ([]byte, error) {
	command := exec.Command(filepath.Join(tools, name), args...)
	var stderr strings.Builder
	command.Stderr = &stderr
	data, err := command.Output()
	if err != nil {
		return nil, fmt.Errorf("%s: %w: %s", name, err, stderr.String())
	}
	return data, nil
}

func fileSHA(path string) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	hash := sha256.New()
	if _, err = io.Copy(hash, file); err != nil {
		return "", err
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

func safePath(name string) bool {
	return name == "" || (fs.ValidPath(name) && !strings.ContainsAny(name, "\\\x00\r\n") && utf8.ValidString(name))
}

func readEROFS(tools, image, work string) (*imageTree, error) {
	data, err := invoke(tools, "erofs-meta", image)
	if err != nil {
		return nil, err
	}
	tree := &imageTree{Entries: make(map[string]*entry)}
	decoder := json.NewDecoder(strings.NewReader(string(data)))
	var first inode
	if err = decoder.Decode(&first); err != nil || first.Filesystem == nil {
		return nil, fmt.Errorf("missing EROFS filesystem metadata: %v", err)
	}
	tree.FS = *first.Filesystem
	if tree.FS.BlockSize < 512 || tree.FS.BlockSize > 4096 || tree.FS.BlockSize&(tree.FS.BlockSize-1) != 0 {
		return nil, fmt.Errorf("unsupported EROFS block size %d", tree.FS.BlockSize)
	}
	for {
		var meta inode
		if err = decoder.Decode(&meta); err == io.EOF {
			break
		}
		if err != nil || meta.Filesystem != nil {
			return nil, fmt.Errorf("invalid EROFS inode metadata: %v", err)
		}
		decoded, err := hex.DecodeString(meta.PathHex)
		name := string(decoded)
		if err != nil || !safePath(name) {
			return nil, fmt.Errorf("invalid EROFS path %q", name)
		}
		if _, exists := tree.Entries[name]; exists {
			return nil, fmt.Errorf("duplicate EROFS path %q", name)
		}
		if meta.Mtime < 0 || meta.MtimeNsec < 0 || meta.MtimeNsec >= 1000000000 || meta.Size > 1<<32 {
			return nil, fmt.Errorf("unsupported inode size or timestamp: %q", name)
		}
		for key, value := range meta.Xattrs {
			keyBytes, keyErr := hex.DecodeString(key)
			_, valueErr := hex.DecodeString(value)
			if keyErr != nil || valueErr != nil || len(keyBytes) == 0 ||
				!utf8.Valid(keyBytes) || strings.ContainsAny(string(keyBytes), "=\n\x00") {
				return nil, fmt.Errorf("unsupported xattr encoding: %q", name)
			}
		}
		tree.Entries[name] = &entry{Meta: meta, Source: filepath.Join(work, name)}
	}
	if root, ok := tree.Entries[""]; !ok || root.Meta.Mode&0170000 != 0040000 {
		return nil, fmt.Errorf("missing EROFS root directory")
	}
	// Verify every parent before allowing the extractor to create any symlinks.
	for name := range tree.Entries {
		if name == "" {
			continue
		}
		parent := filepath.ToSlash(filepath.Dir(name))
		if parent == "." {
			parent = ""
		}
		if node, ok := tree.Entries[parent]; !ok || node.Meta.Mode&0170000 != 0040000 {
			return nil, fmt.Errorf("EROFS parent is not a directory: %q", name)
		}
	}
	if _, err = os.Stat(work); !os.IsNotExist(err) {
		return nil, fmt.Errorf("extraction directory must be new: %s", work)
	}
	if _, err = invoke(tools, "fsck.erofs", "--extract="+work, "--no-preserve", "--no-xattrs", image); err != nil {
		return nil, err
	}
	for name, node := range tree.Entries {
		info, err := os.Lstat(node.Source)
		if err != nil {
			return nil, err
		}
		switch node.Meta.Mode & 0170000 {
		case 0100000:
			if !info.Mode().IsRegular() || uint64(info.Size()) != node.Meta.Size {
				return nil, fmt.Errorf("extracted file differs: %q", name)
			}
			node.Digest, err = fileSHA(node.Source)
		case 0040000:
			if !info.IsDir() {
				return nil, fmt.Errorf("extracted directory differs: %q", name)
			}
		case 0120000:
			if info.Mode()&os.ModeSymlink == 0 {
				return nil, fmt.Errorf("extracted symlink differs: %q", name)
			}
			node.Link, err = os.Readlink(node.Source)
			if uint64(len(node.Link)) != node.Meta.Size {
				return nil, fmt.Errorf("symlink size differs: %q", name)
			}
		default:
			return nil, fmt.Errorf("unsupported inode type: %q", name)
		}
		if err != nil {
			return nil, err
		}
	}
	return tree, nil
}

func sortedNames(tree *imageTree) []string {
	names := make([]string, 0, len(tree.Entries))
	for name := range tree.Entries {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

func writeTreeTar(tree *imageTree, output string) error {
	file, err := os.OpenFile(output, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	defer file.Close()
	writer := tar.NewWriter(file)
	links := make(map[uint64]string)
	for _, name := range sortedNames(tree) {
		node := tree.Entries[name]
		meta := node.Meta
		tarName := name
		if name == "" {
			tarName = "."
		}
		header := &tar.Header{Name: tarName, Mode: int64(meta.Mode & 07777), Uid: int(meta.UID), Gid: int(meta.GID),
			ModTime: time.Unix(meta.Mtime, meta.MtimeNsec), Format: tar.FormatPAX, PAXRecords: make(map[string]string)}
		for keyHex, valueHex := range meta.Xattrs {
			key, _ := hex.DecodeString(keyHex)
			value, _ := hex.DecodeString(valueHex)
			header.PAXRecords["SCHILY.xattr."+string(key)] = string(value)
		}
		switch meta.Mode & 0170000 {
		case 0040000:
			header.Typeflag = tar.TypeDir
		case 0120000:
			header.Typeflag = tar.TypeSymlink
			header.Linkname = node.Link
		case 0100000:
			header.Typeflag = tar.TypeReg
			header.Size = int64(meta.Size)
			if first, exists := links[meta.NID]; exists && meta.NID != 0 {
				header.Typeflag = tar.TypeLink
				header.Linkname = first
				header.Size = 0
			} else if meta.NID != 0 {
				links[meta.NID] = tarName
			}
		default:
			return fmt.Errorf("unsupported tar inode: %q", name)
		}
		if err = writer.WriteHeader(header); err != nil {
			return err
		}
		if header.Typeflag == tar.TypeReg {
			input, err := os.Open(node.Source)
			if err != nil {
				return err
			}
			_, err = io.CopyN(writer, input, header.Size)
			input.Close()
			if err != nil {
				return err
			}
		}
	}
	if err = writer.Close(); err != nil {
		return err
	}
	return file.Close()
}

func sameTree(expected, actual *imageTree) error {
	if expected.FS != actual.FS {
		return fmt.Errorf("EROFS filesystem identity changed: expected=%+v actual=%+v", expected.FS, actual.FS)
	}
	if len(expected.Entries) != len(actual.Entries) {
		return fmt.Errorf("EROFS file inventory changed: expected=%v actual=%v", sortedNames(expected), sortedNames(actual))
	}
	for name, want := range expected.Entries {
		got, ok := actual.Entries[name]
		if !ok {
			return fmt.Errorf("missing rebuilt EROFS entry: %q", name)
		}
		x, y := want.Meta, got.Meta
		x.NID, y.NID = 0, 0
		if x.Mode&0170000 == 0040000 {
			x.Size, y.Size = 0, 0
		}
		if !reflect.DeepEqual(x, y) || want.Digest != got.Digest || want.Link != got.Link {
			return fmt.Errorf("rebuilt EROFS data or metadata differs: %q", name)
		}
	}
	// Preserve hardlink relationships, independently of changed inode numbers.
	for _, tree := range []*imageTree{expected, actual} {
		groups := make(map[uint64][]string)
		for name, e := range tree.Entries {
			if e.Meta.Mode&0170000 == 0100000 && e.Meta.NID != 0 {
				groups[e.Meta.NID] = append(groups[e.Meta.NID], name)
			}
		}
		other := actual
		if tree == actual {
			other = expected
		}
		for _, names := range groups {
			if len(names) < 2 {
				continue
			}
			first := other.Entries[names[0]].Meta.NID
			for _, name := range names {
				if other.Entries[name].Meta.NID != first {
					return fmt.Errorf("hardlink relationship changed: %q", name)
				}
			}
		}
	}
	return nil
}

func writeEROFS(tools string, tree *imageTree, output, work string, compression ...string) error {
	archive := filepath.Join(work, "filesystem.tar")
	if err := writeTreeTar(tree, archive); err != nil {
		return err
	}
	defer os.Remove(archive)
	uuid, err := hex.DecodeString(tree.FS.UUID)
	if err != nil || len(uuid) != 16 {
		return fmt.Errorf("invalid EROFS UUID")
	}
	label, err := hex.DecodeString(tree.FS.Label)
	if err != nil || len(label) != 16 {
		return fmt.Errorf("invalid EROFS volume label")
	}
	volume := strings.TrimRight(string(label), "\x00")
	if len(volume) > 15 || strings.ContainsRune(volume, 0) {
		return fmt.Errorf("unsupported EROFS volume label")
	}
	algorithm := "lz4hc,level=12"
	if len(compression) != 0 {
		algorithm = compression[0]
	}
	args := []string{"--tar=f", "--preserve-mtime", "--workers=2", "-C65536", "-z" + algorithm, "-b" + fmt.Sprint(tree.FS.BlockSize),
		"-U" + fmt.Sprintf("%x-%x-%x-%x-%x", uuid[:4], uuid[4:6], uuid[6:8], uuid[8:10], uuid[10:]), "-L", volume, output, archive}
	if _, err = invoke(tools, "mkfs.erofs", args...); err != nil {
		return err
	}
	check, err := readEROFS(tools, output, filepath.Join(work, "checked"))
	if err != nil {
		return err
	}
	return sameTree(tree, check)
}

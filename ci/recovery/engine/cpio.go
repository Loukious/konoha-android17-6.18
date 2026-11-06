// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

type cpioEntry struct {
	Name   string
	Magic  string
	Fields [13]uint32
	Data   []byte
}

func cpioName(name string) string { return strings.TrimPrefix(name, "./") }

func readCPIO(data []byte) ([]cpioEntry, error) {
	var entries []cpioEntry
	seen := make(map[string]bool)
	for off := uint64(0); off < uint64(len(data)); {
		if uint64(len(data))-off < 110 {
			return nil, fmt.Errorf("truncated cpio header")
		}
		header := data[off : off+110]
		e := cpioEntry{Magic: string(header[:6])}
		if e.Magic != "070701" && e.Magic != "070702" {
			return nil, fmt.Errorf("unsupported cpio format")
		}
		for i := range e.Fields {
			n, err := strconv.ParseUint(string(header[6+i*8:14+i*8]), 16, 32)
			if err != nil {
				return nil, fmt.Errorf("invalid cpio header")
			}
			e.Fields[i] = uint32(n)
		}
		nsize, size := uint64(e.Fields[11]), uint64(e.Fields[6])
		off += 110
		if nsize < 1 || nsize > 4096 || off+nsize > uint64(len(data)) || data[off+nsize-1] != 0 {
			return nil, fmt.Errorf("invalid cpio path bounds")
		}
		e.Name = string(data[off : off+nsize-1])
		off = aligned(off+nsize, 4)
		if off+size > uint64(len(data)) {
			return nil, fmt.Errorf("invalid cpio data bounds")
		}
		e.Data = data[off : off+size]
		off = aligned(off+size, 4)
		if e.Name == "TRAILER!!!" {
			if size != 0 || off > uint64(len(data)) || !allZero(data[off:]) {
				return nil, fmt.Errorf("concatenated or nonzero cpio trailer is unsupported")
			}
			return entries, nil
		}
		name := cpioName(e.Name)
		if name != "." && !safePath(name) || name == "" || seen[name] {
			return nil, fmt.Errorf("unsafe or duplicate cpio path %q", e.Name)
		}
		seen[name] = true
		if e.Magic == "070702" {
			var sum uint32
			for _, b := range e.Data {
				sum += uint32(b)
			}
			if sum != e.Fields[12] {
				return nil, fmt.Errorf("cpio checksum mismatch: %s", name)
			}
		}
		entries = append(entries, e)
	}
	return nil, fmt.Errorf("cpio trailer is missing")
}

func allZero(data []byte) bool {
	for _, b := range data {
		if b != 0 {
			return false
		}
	}
	return true
}

func writeCPIO(entries []cpioEntry) ([]byte, error) {
	var out bytes.Buffer
	for _, e := range append(append([]cpioEntry{}, entries...), cpioEntry{Name: "TRAILER!!!", Magic: "070701"}) {
		if len(e.Data) > 1<<32-1 {
			return nil, fmt.Errorf("cpio file is too large")
		}
		e.Fields[6], e.Fields[11], e.Fields[12] = uint32(len(e.Data)), uint32(len(e.Name)+1), 0
		if e.Magic == "070702" {
			for _, b := range e.Data {
				e.Fields[12] += uint32(b)
			}
		}
		out.WriteString(e.Magic)
		for _, field := range e.Fields {
			fmt.Fprintf(&out, "%08x", field)
		}
		out.WriteString(e.Name)
		out.WriteByte(0)
		for out.Len()%4 != 0 {
			out.WriteByte(0)
		}
		out.Write(e.Data)
		for out.Len()%4 != 0 {
			out.WriteByte(0)
		}
	}
	for out.Len()%512 != 0 {
		out.WriteByte(0)
	}
	return out.Bytes(), nil
}

func (p *kernelPackage) replaceCPIO(data []byte, role, work string) ([]byte, bool, error) {
	entries, err := readCPIO(data)
	if err != nil {
		return nil, false, err
	}
	old := moduleDirectory{Config: make(map[string][]byte), Data: make(map[string][]byte)}
	var template *cpioEntry
	var highest uint32
	for i := range entries {
		e := &entries[i]
		name := cpioName(e.Name)
		if e.Fields[0] > highest {
			highest = e.Fields[0]
		}
		if strings.HasSuffix(name, ".ko") && !strings.HasPrefix(name, "lib/modules/") {
			return nil, false, fmt.Errorf("module outside supported ramdisk directory: %s", name)
		}
		if !strings.HasPrefix(name, "lib/modules/") {
			continue
		}
		file := strings.TrimPrefix(name, "lib/modules/")
		if strings.Contains(file, "/") || e.Fields[1]&0170000 != 0100000 || e.Fields[4] != 1 {
			return nil, false, fmt.Errorf("ramdisk requires a flat regular module layout: %s", name)
		}
		if strings.HasSuffix(file, ".ko") {
			old.Names = append(old.Names, file)
			old.Data[file] = e.Data
			template = e
		} else {
			old.Config[file] = e.Data
		}
	}
	if template == nil {
		return data, false, nil
	}
	files, err := p.moduleFiles(role, old, work)
	if err != nil {
		return nil, false, err
	}
	var updated []cpioEntry
	originals := make(map[string]cpioEntry)
	for _, e := range entries {
		name := cpioName(e.Name)
		if strings.HasPrefix(name, "lib/modules/") {
			originals[filepath.Base(name)] = e
		} else {
			updated = append(updated, e)
		}
	}
	names := make([]string, 0, len(files))
	for name := range files {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		e, ok := originals[name]
		if !ok {
			e = *template
			if highest == ^uint32(0) {
				return nil, false, fmt.Errorf("cpio inode space exhausted")
			}
			highest++
			e.Fields[0] = highest
			e.Name = "lib/modules/" + name
			if strings.HasPrefix(template.Name, "./") {
				e.Name = "./" + e.Name
			}
		}
		e.Data, err = os.ReadFile(files[name])
		if err != nil {
			return nil, false, err
		}
		updated = append(updated, e)
	}
	output, err := writeCPIO(updated)
	if err != nil {
		return nil, false, err
	}
	check, err := readCPIO(output)
	if err != nil {
		return nil, false, err
	}
	if len(check) != len(updated) {
		return nil, false, fmt.Errorf("rebuilt cpio inventory changed")
	}
	// Compare the serialized result entry by entry, including device numbers,
	// owners, modes, hardlink identifiers, names and every retained byte.
	for i, want := range updated {
		got := check[i]
		want.Fields[6], want.Fields[11], want.Fields[12] = got.Fields[6], got.Fields[11], got.Fields[12]
		if want.Name != got.Name || want.Magic != got.Magic || want.Fields != got.Fields || !bytes.Equal(want.Data, got.Data) {
			return nil, false, fmt.Errorf("rebuilt cpio data or metadata differs: %s", want.Name)
		}
	}
	return output, true, nil
}

func (p *kernelPackage) replaceRamdisk(tools string, data []byte, role, work string) ([]byte, bool, error) {
	compression := ""
	switch {
	case bytes.HasPrefix(data, []byte("07070")):
	case bytes.HasPrefix(data, []byte{0x1f, 0x8b}):
		compression = "gzip"
	case bytes.HasPrefix(data, []byte{0x02, 0x21, 0x4c, 0x18}):
		compression = "lz4_legacy"
	case bytes.HasPrefix(data, []byte{0x04, 0x22, 0x4d, 0x18}):
		compression = "lz4"
	default:
		return nil, false, fmt.Errorf("unsupported %s ramdisk compression", role)
	}
	if err := os.Mkdir(work, 0700); err != nil {
		return nil, false, err
	}
	input, raw := filepath.Join(work, "original"), filepath.Join(work, "original.cpio")
	if err := os.WriteFile(input, data, 0600); err != nil {
		return nil, false, err
	}
	if compression != "" {
		if _, err := invoke(tools, "magiskboot", "decompress", input, raw); err != nil {
			return nil, false, err
		}
		var err error
		data, err = os.ReadFile(raw)
		if err != nil {
			return nil, false, err
		}
	}
	updated, changed, err := p.replaceCPIO(data, role, filepath.Join(work, "modules"))
	if err != nil {
		return nil, false, err
	}
	if !changed {
		data, err = os.ReadFile(input)
		return data, false, err
	}
	if compression == "" {
		return updated, true, nil
	}
	newRaw, compressed := filepath.Join(work, "updated.cpio"), filepath.Join(work, "updated")
	if err = os.WriteFile(newRaw, updated, 0600); err != nil {
		return nil, false, err
	}
	if _, err = invoke(tools, "magiskboot", "compress="+compression, newRaw, compressed); err != nil {
		return nil, false, err
	}
	if _, err = invoke(tools, "magiskboot", "decompress", compressed, filepath.Join(work, "checked.cpio")); err != nil {
		return nil, false, err
	}
	check, err := os.ReadFile(filepath.Join(work, "checked.cpio"))
	if err != nil || !bytes.Equal(check, updated) {
		return nil, false, fmt.Errorf("ramdisk compression roundtrip failed: %v", err)
	}
	result, err := os.ReadFile(compressed)
	return result, true, err
}

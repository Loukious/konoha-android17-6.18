// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// Android liblp on-disk structures, not a writable partition implementation.
// Besides checking the active mappings, reject extents shared with any other
// logical partition or the inactive slot before permitting a recovery write.
type lpExtent struct{ Start, Count uint64 }
type lpPartition struct {
	Name    string
	Extents []lpExtent
}
type lpMetadata struct {
	Partitions []lpPartition
	Digest     string
}

var lpName = regexp.MustCompile(`^[a-zA-Z0-9_-]+$`)

func nullString(data []byte) (string, error) {
	n := bytes.IndexByte(data, 0)
	if n < 0 {
		n = len(data)
	}
	if !allZero(data[n:]) {
		return "", fmt.Errorf("invalid metadata string padding")
	}
	name := string(data[:n])
	if !lpName.MatchString(name) {
		return "", fmt.Errorf("invalid logical partition name")
	}
	return name, nil
}

func lpChecksum(data []byte, offset int) bool {
	if len(data) < offset+32 {
		return false
	}
	copyData := append([]byte{}, data...)
	clear(copyData[offset : offset+32])
	sum := sha256.Sum256(copyData)
	return bytes.Equal(sum[:], data[offset:offset+32])
}

func readLP(super string, slot int) (*lpMetadata, error) {
	f, err := os.Open(super)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	geometry := make([]byte, 4096)
	backupGeometry := make([]byte, 4096)
	if _, err = f.ReadAt(geometry, 4096); err != nil {
		return nil, err
	}
	if _, err = f.ReadAt(backupGeometry, 8192); err != nil {
		return nil, err
	}
	if !bytes.Equal(geometry, backupGeometry) || le.Uint32(geometry) != 0x616c4467 || le.Uint32(geometry[4:]) != 52 || !lpChecksum(geometry[:52], 8) {
		return nil, fmt.Errorf("invalid or inconsistent super geometry")
	}
	maximum, count, block := uint64(le.Uint32(geometry[40:])), uint64(le.Uint32(geometry[44:])), uint64(le.Uint32(geometry[48:]))
	if maximum < 4096 || maximum > 4<<20 || maximum%512 != 0 || count < 2 || count > 4 || slot < 0 || uint64(slot) >= count || block != 4096 {
		return nil, fmt.Errorf("unsupported super geometry layout")
	}
	data, backup := make([]byte, maximum), make([]byte, maximum)
	offset := int64(12288 + uint64(slot)*maximum)
	if _, err = f.ReadAt(data, offset); err != nil {
		return nil, err
	}
	if _, err = f.ReadAt(backup, offset+int64(count*maximum)); err != nil {
		return nil, err
	}
	if !bytes.Equal(data, backup) {
		return nil, fmt.Errorf("primary and backup logical metadata differ")
	}
	if le.Uint32(data) != 0x414c5030 || le.Uint16(data[4:]) != 10 || le.Uint16(data[6:]) > 2 {
		return nil, fmt.Errorf("unsupported logical metadata version")
	}
	headerSize, tablesSize := uint64(le.Uint32(data[8:])), uint64(le.Uint32(data[44:]))
	if (headerSize != 128 && headerSize != 256) || headerSize+tablesSize > maximum || !lpChecksum(data[:headerSize], 12) {
		return nil, fmt.Errorf("invalid logical metadata header")
	}
	tables := data[headerSize : headerSize+tablesSize]
	sum := sha256.Sum256(tables)
	if !bytes.Equal(sum[:], data[48:80]) {
		return nil, fmt.Errorf("logical metadata table checksum mismatch")
	}
	table := func(index uint64, size uint64) ([][]byte, error) {
		desc := data[80+index*12 : 92+index*12]
		off, n, stride := uint64(le.Uint32(desc)), uint64(le.Uint32(desc[4:])), uint64(le.Uint32(desc[8:]))
		if stride != size || n > 65536 || off > tablesSize || n*stride > tablesSize-off {
			return nil, fmt.Errorf("invalid logical metadata table bounds")
		}
		var rows [][]byte
		for i := uint64(0); i < n; i++ {
			rows = append(rows, tables[off+i*stride:off+(i+1)*stride])
		}
		return rows, nil
	}
	parts, err := table(0, 52)
	if err != nil {
		return nil, err
	}
	extents, err := table(1, 24)
	if err != nil {
		return nil, err
	}
	groups, err := table(2, 48)
	if err != nil {
		return nil, err
	}
	devices, err := table(3, 64)
	if err != nil {
		return nil, err
	}
	if len(devices) != 1 {
		return nil, fmt.Errorf("only a single physical super device is supported")
	}
	device := devices[0]
	first, size := le.Uint64(device), le.Uint64(device[16:])/512
	deviceName, err := nullString(device[24:60])
	if err != nil || deviceName != "super" || le.Uint32(device[60:]) != 0 || size <= first {
		return nil, fmt.Errorf("unexpected physical super device")
	}
	result := &lpMetadata{}
	fullSum := sha256.Sum256(data[:headerSize+tablesSize])
	result.Digest = hex.EncodeToString(fullSum[:])
	seen := make(map[string]bool)
	for _, row := range parts {
		name, err := nullString(row[:36])
		if err != nil {
			return nil, err
		}
		flags, start, n, group := le.Uint32(row[36:]), uint64(le.Uint32(row[40:])), uint64(le.Uint32(row[44:])), uint64(le.Uint32(row[48:]))
		if flags & ^uint32(15) != 0 || start > uint64(len(extents)) || n > uint64(len(extents))-start || group >= uint64(len(groups)) {
			return nil, fmt.Errorf("invalid logical partition entry")
		}
		if flags&2 != 0 {
			name += []string{"_a", "_b"}[slot]
		}
		if seen[name] {
			return nil, fmt.Errorf("duplicate logical partition name")
		}
		seen[name] = true
		partition := lpPartition{Name: name}
		for _, e := range extents[start : start+n] {
			sectors, kind, target, source := le.Uint64(e), le.Uint32(e[8:]), le.Uint64(e[12:]), le.Uint32(e[20:])
			if sectors == 0 {
				return nil, fmt.Errorf("empty logical extent")
			}
			if kind == 1 {
				continue
			}
			if kind != 0 || source != 0 || target < first || target > size || sectors > size-target {
				return nil, fmt.Errorf("logical extent exceeds physical super bounds")
			}
			partition.Extents = append(partition.Extents, lpExtent{target, sectors})
		}
		result.Partitions = append(result.Partitions, partition)
	}
	return result, nil
}

func overlap(a, b lpExtent) bool { return a.Start < b.Start+b.Count && b.Start < a.Start+a.Count }

func checkLayout(super, slot, maps string) error {
	index := 0
	if slot == "_b" {
		index = 1
	} else if slot != "_a" {
		return fmt.Errorf("invalid active slot")
	}
	var metadata [2]*lpMetadata
	for i := 0; i < 2; i++ {
		var err error
		metadata[i], err = readLP(super, i)
		if err != nil {
			return fmt.Errorf("logical metadata slot %d: %w", i, err)
		}
	}
	for _, base := range []string{"vendor_dlkm", "system_dlkm"} {
		name := base + slot
		var selected *lpPartition
		for i := range metadata[index].Partitions {
			if metadata[index].Partitions[i].Name == name {
				selected = &metadata[index].Partitions[i]
			}
		}
		if selected == nil || len(selected.Extents) == 0 {
			return fmt.Errorf("active logical partition missing: %s", name)
		}
		for i, a := range selected.Extents {
			for _, b := range selected.Extents[i+1:] {
				if overlap(a, b) {
					return fmt.Errorf("logical partition overlaps itself: %s", name)
				}
			}
		}
		for _, meta := range metadata {
			for _, other := range meta.Partitions {
				if other.Name == name {
					continue
				}
				for _, a := range selected.Extents {
					for _, b := range other.Extents {
						if overlap(a, b) {
							return fmt.Errorf("%s shares physical data with %s; a full ROM OTA is required for this layout", name, other.Name)
						}
					}
				}
			}
		}
		text, err := os.ReadFile(filepath.Join(maps, base+"-map.txt"))
		if err != nil {
			return err
		}
		lines := strings.Split(strings.TrimSpace(string(text)), "\n")
		if len(lines) != len(selected.Extents)+1 || !strings.HasPrefix(lines[0], "Mapping: "+name+" ") {
			return fmt.Errorf("active mapping does not match logical metadata: %s", name)
		}
		logical := uint64(0)
		for i, line := range lines[1:] {
			words := strings.Fields(line)
			if len(words) != 5 || words[2] != "linear" {
				return fmt.Errorf("invalid active mapping record")
			}
			start, e1 := strconv.ParseUint(words[0], 10, 64)
			count, e2 := strconv.ParseUint(words[1], 10, 64)
			physical, e3 := strconv.ParseUint(words[4], 10, 64)
			if e1 != nil || e2 != nil || e3 != nil || start != logical || count != selected.Extents[i].Count || physical != selected.Extents[i].Start {
				return fmt.Errorf("%s mapping differs from logical metadata", name)
			}
			logical += count
		}
	}
	fmt.Printf("Active slot %s; separate DLKM extents; metadata %s %s\n", slot, metadata[0].Digest, metadata[1].Digest)
	return nil
}

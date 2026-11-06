// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

var le = binary.LittleEndian
var initOverride = regexp.MustCompile(`(?:^|[\s;])androidboot\.init_rc\s*=`)

type bootImage struct {
	Header                                             []byte
	Page                                               uint64
	Version                                            uint32
	Vendor                                             bool
	Kernel, Ramdisk, DTB, Table, Bootconfig, Signature []byte
}

func parseBoot(data []byte) (*bootImage, error) {
	if len(data) < 4096 {
		return nil, fmt.Errorf("truncated boot image")
	}
	p := &bootImage{}
	var sizes []uint64
	switch string(data[:8]) {
	case "ANDROID!":
		p.Version, p.Page = le.Uint32(data[40:]), 4096
		if p.Version != 3 && p.Version != 4 {
			return nil, fmt.Errorf("only boot/recovery header versions 3 and 4 are supported")
		}
		headerSize := uint64(1580)
		if p.Version == 4 {
			headerSize = 1584
		}
		if uint64(le.Uint32(data[20:])) != headerSize {
			return nil, fmt.Errorf("unexpected boot header size")
		}
		p.Header = append([]byte{}, data[:headerSize]...)
		sizes = []uint64{uint64(le.Uint32(data[8:])), uint64(le.Uint32(data[12:]))}
		if p.Version == 4 {
			sizes = append(sizes, uint64(le.Uint32(data[1580:])))
		}
	case "VNDRBOOT":
		p.Vendor = true
		p.Version, p.Page = le.Uint32(data[8:]), uint64(le.Uint32(data[12:]))
		if (p.Version != 3 && p.Version != 4) || p.Page < 4096 || p.Page > 16384 || p.Page&(p.Page-1) != 0 {
			return nil, fmt.Errorf("unsupported vendor boot header or page size")
		}
		headerSize := uint64(2112)
		if p.Version == 4 {
			headerSize = 2128
		}
		if uint64(le.Uint32(data[2096:])) != headerSize {
			return nil, fmt.Errorf("unexpected vendor boot header size")
		}
		p.Header = append([]byte{}, data[:headerSize]...)
		sizes = []uint64{uint64(le.Uint32(data[24:])), uint64(le.Uint32(data[2100:]))}
		if p.Version == 4 {
			sizes = append(sizes, uint64(le.Uint32(data[2112:])), uint64(le.Uint32(data[2124:])))
		}
	default:
		return nil, fmt.Errorf("unknown boot image magic")
	}
	offset := p.Page
	var parts [][]byte
	for _, size := range sizes {
		if offset > uint64(len(data)) || size > uint64(len(data))-offset {
			return nil, fmt.Errorf("boot image section exceeds image bounds")
		}
		parts = append(parts, data[offset:offset+size])
		offset += aligned(size, p.Page)
	}
	if offset > uint64(len(data)) || !allZero(data[offset:]) {
		return nil, fmt.Errorf("unsupported boot image trailing data")
	}
	if p.Vendor {
		p.Ramdisk, p.DTB = parts[0], parts[1]
		if p.Version == 4 {
			p.Table, p.Bootconfig = append([]byte{}, parts[2]...), parts[3]
		}
	} else {
		p.Kernel, p.Ramdisk = parts[0], parts[1]
		if p.Version == 4 {
			p.Signature = parts[2]
		}
	}
	return p, nil
}

func (p *bootImage) encode() []byte {
	var parts [][]byte
	if p.Vendor {
		le.PutUint32(p.Header[24:], uint32(len(p.Ramdisk)))
		le.PutUint32(p.Header[2100:], uint32(len(p.DTB)))
		parts = [][]byte{p.Ramdisk, p.DTB}
		if p.Version == 4 {
			parts = append(parts, p.Table, p.Bootconfig)
		}
	} else {
		le.PutUint32(p.Header[8:], uint32(len(p.Kernel)))
		le.PutUint32(p.Header[12:], uint32(len(p.Ramdisk)))
		if p.Version == 4 {
			le.PutUint32(p.Header[1580:], uint32(len(p.Signature)))
		}
		parts = [][]byte{p.Kernel, p.Ramdisk}
		if p.Version == 4 {
			parts = append(parts, p.Signature)
		}
	}
	data := append([]byte{}, p.Header...)
	data = append(data, make([]byte, p.Page-uint64(len(data)))...)
	for _, part := range parts {
		data = append(data, part...)
		data = append(data, make([]byte, aligned(uint64(len(part)), p.Page)-uint64(len(part)))...)
	}
	return data
}

func commandLine(field []byte, parameters []string, addMissing bool, firmware bool) error {
	n := bytes.IndexByte(field, 0)
	if n < 0 || !allZero(field[n:]) {
		return fmt.Errorf("unsupported boot command line encoding")
	}
	words := strings.Fields(string(field[:n]))
	if initOverride.Match(field[:n]) {
		return fmt.Errorf("external init override requires a ROM-specific migration")
	}
	replacements := make(map[string]string)
	var keys []string
	for _, parameter := range parameters {
		key, _, ok := strings.Cut(parameter, "=")
		if !ok || (key != "android_arch_task_struct_size" && key != "sched_proxy_exec") || strings.ContainsAny(parameter, " \t\r\n\x00") || replacements[key] != "" {
			return fmt.Errorf("unreviewed kernel boot parameter %q", parameter)
		}
		keys = append(keys, key)
		replacements[key] = parameter
	}
	seen := make(map[string]bool)
	var updated []string
	for _, word := range words {
		key, value, _ := strings.Cut(word, "=")
		if replacement, ok := replacements[key]; ok {
			if !seen[key] {
				updated = append(updated, replacement)
				seen[key] = true
			}
		} else if firmware && key == "firmware_class.path" {
			if seen[key] || strings.ContainsAny(value, "\"'\r\n") {
				return fmt.Errorf("unsupported firmware search parameter")
			}
			paths := []string{"/vendor_dlkm/lib/firmware"}
			for _, path := range strings.Split(value, ",") {
				if path == "/vendor_dlkm/lib/firmware" {
					continue
				}
				if !strings.HasPrefix(path, "/") {
					return fmt.Errorf("unsupported firmware search path")
				}
				paths = append(paths, path)
			}
			if len(paths) > 10 {
				return fmt.Errorf("too many firmware search paths")
			}
			updated = append(updated, key+"="+strings.Join(paths, ","))
			seen[key] = true
		} else {
			updated = append(updated, word)
		}
	}
	if addMissing {
		for _, key := range keys {
			if !seen[key] {
				updated = append(updated, replacements[key])
			}
		}
	}
	if firmware && !seen["firmware_class.path"] {
		updated = append(updated, "firmware_class.path=/vendor_dlkm/lib/firmware")
	}
	text := strings.Join(updated, " ")
	if len(text) >= len(field) {
		return fmt.Errorf("updated boot command line exceeds header capacity")
	}
	clear(field)
	copy(field, text)
	return nil
}

func (pkg *kernelPackage) rebuildBoot(tools, role, original, target, work string) error {
	avb, err := readAVB(original)
	if err != nil {
		return err
	}
	data, err := os.ReadFile(original)
	if err != nil {
		return err
	}
	image, err := parseBoot(data[:avb.OriginalSize])
	if err != nil {
		return fmt.Errorf("%s: %w", role, err)
	}
	if image.Vendor != (role == "vendor_boot") {
		return fmt.Errorf("unexpected %s image type", role)
	}
	if err = os.Mkdir(work, 0700); err != nil {
		return err
	}
	if image.Vendor {
		// Retain the ROM's bootconfig byte for byte, including parallel loading.
		if initOverride.Match(image.Bootconfig) {
			return fmt.Errorf("external init override requires a ROM-specific migration")
		}
		if err = commandLine(image.Header[28:2076], pkg.Parameters, true, len(pkg.Firmware) > 0); err != nil {
			return err
		}
		image.DTB, err = os.ReadFile(filepath.Join(pkg.Root, "payload/dtb.img"))
		if err != nil {
			return err
		}
		if len(image.DTB) < 8 || be.Uint32(image.DTB) != 0xd00dfeed {
			return fmt.Errorf("invalid release DTB")
		}
		if image.Version == 3 {
			var changed bool
			image.Ramdisk, changed, err = pkg.replaceRamdisk(tools, image.Ramdisk, "vendor_ramdisk", filepath.Join(work, "ramdisk"))
			if err != nil {
				return err
			}
			if !changed {
				return fmt.Errorf("vendor ramdisk has no modules")
			}
		} else {
			count, stride := uint64(le.Uint32(image.Header[2116:])), uint64(le.Uint32(image.Header[2120:]))
			if count == 0 || count > 64 || stride != 108 || count*stride != uint64(len(image.Table)) {
				return fmt.Errorf("unsupported vendor ramdisk table")
			}
			var ramdisk []byte
			previousEnd := uint64(0)
			moduleFragments := 0
			for i := uint64(0); i < count; i++ {
				row := image.Table[i*stride : (i+1)*stride]
				size, offset := uint64(le.Uint32(row)), uint64(le.Uint32(row[4:]))
				if offset != previousEnd || size == 0 || offset > uint64(len(image.Ramdisk)) || size > uint64(len(image.Ramdisk))-offset {
					return fmt.Errorf("unsupported vendor ramdisk fragment layout")
				}
				fragment, changed, err := pkg.replaceRamdisk(tools, image.Ramdisk[offset:offset+size], "vendor_ramdisk", filepath.Join(work, fmt.Sprintf("fragment%d", i)))
				if err != nil {
					return err
				}
				if changed {
					moduleFragments++
				}
				le.PutUint32(row, uint32(len(fragment)))
				le.PutUint32(row[4:], uint32(len(ramdisk)))
				ramdisk = append(ramdisk, fragment...)
				previousEnd = offset + size
			}
			if previousEnd != uint64(len(image.Ramdisk)) || moduleFragments != 1 {
				return fmt.Errorf("expected exactly one vendor fragment containing a complete module directory")
			}
			image.Ramdisk = ramdisk
		}
	} else {
		if len(image.Kernel) > 0 {
			image.Kernel, err = os.ReadFile(filepath.Join(pkg.Root, "payload/Image"))
			if err != nil {
				return err
			}
			if len(image.Kernel) < 64 || !bytes.Equal(image.Kernel[56:60], []byte{'A', 'R', 'M', 0x64}) {
				return fmt.Errorf("invalid release ARM64 kernel")
			}
		} else if role == "boot" {
			return fmt.Errorf("boot image contains no kernel")
		}
		// The old GKI certificate cannot authenticate the new kernel; the newly
		// generated outer AVB metadata replaces it under the verified flags=3 gate.
		image.Signature = nil
		if err = commandLine(image.Header[44:1580], pkg.Parameters, false, false); err != nil {
			return err
		}
		if len(image.Ramdisk) > 0 {
			var changed bool
			image.Ramdisk, changed, err = pkg.replaceRamdisk(tools, image.Ramdisk, role, filepath.Join(work, "ramdisk"))
			if err != nil {
				return err
			}
			if role == "recovery" {
				if changed {
					pkg.RecoveryModules = "recovery"
				} else if pkg.VendorRecovery {
					// Some Onyx ROMs keep recovery's module list and drivers in
					// vendor_boot. That directory has already been rebuilt and checked.
					pkg.RecoveryModules = "vendor_boot"
					fmt.Println("Recovery uses the matched vendor ramdisk recovery modules")
				} else {
					return fmt.Errorf("recovery has no compatible module directory or vendor recovery load list")
				}
			}
		} else if role == "recovery" {
			return fmt.Errorf("recovery has no ramdisk")
		}
	}
	encoded := image.encode()
	check, err := parseBoot(encoded)
	if err != nil {
		return err
	}
	if !bytes.Equal(check.Header, image.Header) || !bytes.Equal(check.Kernel, image.Kernel) || !bytes.Equal(check.Ramdisk, image.Ramdisk) ||
		!bytes.Equal(check.DTB, image.DTB) || !bytes.Equal(check.Table, image.Table) || !bytes.Equal(check.Bootconfig, image.Bootconfig) {
		return fmt.Errorf("%s boot image roundtrip failed", role)
	}
	if err = os.WriteFile(target, encoded, 0600); err != nil {
		return err
	}
	return writeAVB(original, target, role)
}

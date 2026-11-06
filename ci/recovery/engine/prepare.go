// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

func (p *kernelPackage) rebuildDLKM(tools, role, original, target, work string) error {
	if err := os.Mkdir(work, 0700); err != nil {
		return err
	}
	tree, err := readEROFS(tools, original, filepath.Join(work, "original"))
	if err != nil {
		return err
	}
	old := moduleDirectory{Config: make(map[string][]byte), Sources: make(map[string]string)}
	var template *entry
	for _, name := range sortedNames(tree) {
		node := tree.Entries[name]
		if strings.HasSuffix(name, ".ko") && !strings.HasPrefix(name, "lib/modules/") {
			return fmt.Errorf("%s has modules outside lib/modules", role)
		}
		if !strings.HasPrefix(name, "lib/modules/") {
			continue
		}
		file := strings.TrimPrefix(name, "lib/modules/")
		if strings.Contains(file, "/") || node.Meta.Mode&0170000 != 0100000 {
			return fmt.Errorf("%s requires a flat regular module directory: %s", role, name)
		}
		if strings.HasSuffix(file, ".ko") {
			old.Names = append(old.Names, file)
			old.Sources[file] = node.Source
			if template == nil {
				template = node
			}
		} else {
			if node.Meta.Size > 1<<20 {
				return fmt.Errorf("unexpectedly large module configuration")
			}
			old.Config[file], err = os.ReadFile(node.Source)
			if err != nil {
				return err
			}
		}
	}
	if template == nil {
		return fmt.Errorf("%s has no modules", role)
	}
	files, err := p.moduleFiles(role, old, filepath.Join(work, "modules"))
	if err != nil {
		return err
	}
	// Do not break a hardlink from a replaced module/config into unrelated ROM data.
	moduleInodes := make(map[uint64]bool)
	for name, node := range tree.Entries {
		if strings.HasPrefix(name, "lib/modules/") {
			moduleInodes[node.Meta.NID] = true
		}
	}
	for name, node := range tree.Entries {
		if !strings.HasPrefix(name, "lib/modules/") && node.Meta.Mode&0170000 == 0100000 && moduleInodes[node.Meta.NID] {
			return fmt.Errorf("module hardlink crosses directory boundary: %s", name)
		}
	}
	originalEntries := make(map[string]*entry)
	for name, node := range tree.Entries {
		if strings.HasPrefix(name, "lib/modules/") {
			originalEntries[filepath.Base(name)] = node
			delete(tree.Entries, name)
		}
	}
	for name, source := range files {
		base := template
		if old, ok := originalEntries[name]; ok {
			base = old
		}
		node, err := replacementEntry(base, "lib/modules/"+name, source)
		if err != nil {
			return err
		}
		tree.Entries["lib/modules/"+name] = node
	}
	if role == "vendor_dlkm" {
		for _, file := range p.Firmware {
			name := "lib/firmware/" + file
			if err = ensureTreeDirectory(tree, filepath.ToSlash(filepath.Dir(name))); err != nil {
				return err
			}
			base := tree.Entries[filepath.ToSlash(filepath.Dir(name))]
			if existing, ok := tree.Entries[name]; ok {
				if existing.Meta.Mode&0170000 != 0100000 {
					return fmt.Errorf("firmware target is not a regular file: %s", name)
				}
				base = existing
			}
			node, err := replacementEntry(base, name, filepath.Join(p.Root, "payload/firmware", file))
			if err != nil {
				return err
			}
			if base.Meta.Mode&0170000 == 0040000 {
				node.Meta.Mode = 0100644
			}
			tree.Entries[name] = node
		}
	}
	if err = writeEROFS(tools, tree, target, work, p.Compression); err != nil {
		return err
	}
	return writeAVB(original, target, role)
}

func replacementEntry(base *entry, name, source string) (*entry, error) {
	info, err := os.Stat(source)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("replacement is not a regular file")
	}
	meta := base.Meta
	meta.NID = 0
	meta.Size = uint64(info.Size())
	meta.PathHex = hex.EncodeToString([]byte(name))
	digest, err := fileSHA(source)
	if err != nil {
		return nil, err
	}
	return &entry{Meta: meta, Source: source, Digest: digest}, nil
}

func ensureTreeDirectory(tree *imageTree, name string) error {
	if node, ok := tree.Entries[name]; ok {
		if node.Meta.Mode&0170000 != 0040000 {
			return fmt.Errorf("filesystem parent is not a directory: %s", name)
		}
		return nil
	}
	parent := filepath.ToSlash(filepath.Dir(name))
	if parent == "." {
		parent = ""
	}
	if err := ensureTreeDirectory(tree, parent); err != nil {
		return err
	}
	meta := tree.Entries[parent].Meta
	meta.NID = 0
	meta.Size = 0
	meta.Mode = 0040755
	meta.PathHex = hex.EncodeToString([]byte(name))
	tree.Entries[name] = &entry{Meta: meta}
	return nil
}

func prepareImages(packageRoot, input, output, work string) error {
	p, err := loadPackage(packageRoot)
	if err != nil {
		return err
	}
	// A generic package has no private signing key for another ROM. It only
	// supports the ROM's existing unlocked/AVB-disabled configuration and never
	// modifies vbmeta or disables enforcement itself.
	vbmeta, err := os.ReadFile(filepath.Join(input, "vbmeta.img"))
	if err != nil {
		return err
	}
	if len(vbmeta) < 256 || string(vbmeta[:4]) != "AVB0" || be.Uint32(vbmeta[120:])&3 != 3 {
		return fmt.Errorf("this ROM must already have both AVB verification and verity disabled")
	}
	for _, dir := range []string{output, work} {
		if err = os.Mkdir(dir, 0700); err != nil {
			return err
		}
	}
	tools := filepath.Join(packageRoot, "tools")
	parts := []string{"system_dlkm", "vendor_dlkm", "dtbo", "vendor_boot", "recovery", "boot"}
	receipt := make(map[string]map[string]any)
	var plan strings.Builder
	for _, part := range parts {
		original, target := filepath.Join(input, part+".img"), filepath.Join(output, part+".img")
		info, err := os.Lstat(original)
		if err != nil || !info.Mode().IsRegular() {
			return fmt.Errorf("input must be a regular partition dump: %s", part)
		}
		fmt.Printf("Rebuilding %s from the installed ROM\n", part)
		switch part {
		case "system_dlkm", "vendor_dlkm":
			err = p.rebuildDLKM(tools, part, original, target, filepath.Join(work, part))
		case "boot", "vendor_boot", "recovery":
			err = p.rebuildBoot(tools, part, original, target, filepath.Join(work, part))
		case "dtbo":
			var data []byte
			data, err = os.ReadFile(filepath.Join(packageRoot, "payload/dtbo.img"))
			if err == nil && (len(data) < 32 || be.Uint32(data) != 0xd7b7ab1e || be.Uint32(data[4:]) < 32 ||
				uint64(be.Uint32(data[4:])) > uint64(len(data)) || !allZero(data[be.Uint32(data[4:]):])) {
				err = fmt.Errorf("invalid release DTBO image")
			}
			if err == nil {
				err = os.WriteFile(target, data, 0600)
			}
			if err == nil {
				err = writeAVB(original, target, part)
			}
		}
		if err != nil {
			return fmt.Errorf("%s: %w", part, err)
		}
		after, err := os.Stat(target)
		if err != nil {
			return err
		}
		if after.Size() != info.Size() {
			return fmt.Errorf("%s partition size changed", part)
		}
		beforeSHA, err := fileSHA(original)
		if err != nil {
			return err
		}
		afterSHA, err := fileSHA(target)
		if err != nil {
			return err
		}
		receipt[part] = map[string]any{"original_sha256": beforeSHA, "sha256": afterSHA, "bytes": after.Size()}
		fmt.Fprintf(&plan, "%s\t%d\t%s\t%s\n", part, after.Size(), afterSHA, beforeSHA)
	}
	data, err := json.MarshalIndent(map[string]any{"format": 1, "kernel_release": p.Release, "source_revision": p.Source,
		"partitions": receipt, "retired_legacy_modules": p.Retired, "recovery_modules_partition": p.RecoveryModules}, "", "  ")
	if err != nil {
		return err
	}
	if err = os.WriteFile(filepath.Join(output, "prepared.json"), append(data, '\n'), 0600); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(output, "prepared.tsv"), []byte(plan.String()), 0600)
}

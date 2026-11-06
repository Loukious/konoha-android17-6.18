// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"bytes"
	"debug/elf"
	"fmt"
	"os"
	"regexp"
	"slices"
	"strings"
)

// These names and load profiles come from the recorded working Onyx baseline
// and source-built 6.18 module layout. A filename alone never permits removal.
type legacyProfile struct {
	ModuleNames  []string                       `json:"module_names"`
	DefaultLoads map[string]map[string][]string `json:"default_loads"`
}

var legacyRelease = regexp.MustCompile(`^6\.6\.[0-9]+(?:[-+][^\s]+)?(?: |$)`)

func (p *kernelPackage) validateLegacyProfile() error {
	if p.Legacy == nil {
		return nil
	}
	seen := make(map[string]bool)
	for _, name := range p.Legacy.ModuleNames {
		if !moduleName.MatchString(name) || seen[name] {
			return fmt.Errorf("invalid legacy module identity %q", name)
		}
		seen[name] = true
	}
	for role, lists := range p.Legacy.DefaultLoads {
		if !slices.Contains([]string{"vendor_ramdisk", "vendor_dlkm", "system_dlkm", "recovery", "boot"}, role) {
			return fmt.Errorf("invalid legacy load profile role %s", role)
		}
		for file, names := range lists {
			if file != "modules.load" && file != "modules.load.recovery" {
				return fmt.Errorf("invalid legacy load profile %s", file)
			}
			seen := make(map[string]bool)
			for _, name := range names {
				_, module := p.Modules[name]
				if seen[name] || (!module && !slices.Contains(p.Builtin, name)) {
					return fmt.Errorf("legacy profile has duplicate or unavailable module %s", name)
				}
				seen[name] = true
			}
		}
	}
	return nil
}

func (p *kernelPackage) verifyLegacyModule(old moduleDirectory, file string) error {
	name := normalizeModule(file)
	if p.Legacy == nil || !slices.Contains(p.Legacy.ModuleNames, name) {
		return fmt.Errorf("not a reviewed legacy module")
	}
	data, ok := old.Data[file]
	if !ok {
		path, exists := old.Sources[file]
		if !exists {
			return fmt.Errorf("legacy module binary is missing")
		}
		info, err := os.Lstat(path)
		if err != nil || !info.Mode().IsRegular() || info.Size() > 64<<20 {
			return fmt.Errorf("unsupported legacy module file")
		}
		data, err = os.ReadFile(path)
		if err != nil {
			return err
		}
	}
	if len(data) > 64<<20 {
		return fmt.Errorf("legacy module is too large")
	}
	binary, err := elf.NewFile(bytes.NewReader(data))
	if err != nil {
		return fmt.Errorf("legacy module is not ELF: %w", err)
	}
	defer binary.Close()
	if binary.Class != elf.ELFCLASS64 || binary.Data != elf.ELFDATA2LSB || binary.Machine != elf.EM_AARCH64 || binary.Type != elf.ET_REL {
		return fmt.Errorf("legacy module is not an ARM64 kernel object")
	}
	section := binary.Section(".modinfo")
	if section == nil || section.Size > 1<<20 || section.Flags&elf.SHF_COMPRESSED != 0 {
		return fmt.Errorf("legacy module has no supported modinfo")
	}
	metadata, err := section.Data()
	if err != nil {
		return err
	}
	fields := make(map[string]string)
	for _, record := range bytes.Split(metadata, []byte{0}) {
		key, value, ok := strings.Cut(string(record), "=")
		if !ok || (key != "name" && key != "vermagic") {
			continue
		}
		if _, duplicate := fields[key]; duplicate {
			return fmt.Errorf("duplicate legacy module identity field")
		}
		fields[key] = value
	}
	if fields["name"] != name || !legacyRelease.MatchString(fields["vermagic"]) {
		return fmt.Errorf("legacy module identity or 6.6 vermagic does not match")
	}
	return nil
}

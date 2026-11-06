// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

type kernelModule struct {
	File    string   `json:"file"`
	Kind    string   `json:"kind"`
	Depends []string `json:"depends"`
	Pre     []string `json:"pre"`
	Post    []string `json:"post"`
	Aliases []string `json:"aliases"`
}

type kernelPackage struct {
	Format          int                     `json:"format"`
	Device          string                  `json:"device"`
	Release         string                  `json:"kernel_release"`
	Source          string                  `json:"source_revision"`
	Common          string                  `json:"common_revision"`
	KernelSU        string                  `json:"kernelsu_revision"`
	Modules         map[string]kernelModule `json:"modules"`
	Builtin         []string                `json:"builtin"`
	NativeAdditions []string                `json:"native_additions"`
	Parameters      []string                `json:"boot_parameters"`
	Firmware        []string                `json:"firmware"`
	Compression     string                  `json:"erofs_compression"`
	Legacy          *legacyProfile          `json:"legacy_onyx,omitempty"`
	Hashes          map[string]string       `json:"sha256"`
	Root            string                  `json:"-"`
	Retired         map[string][]string     `json:"-"`
	VendorRecovery  bool                    `json:"-"`
	RecoveryModules string                  `json:"-"`
}

var moduleName = regexp.MustCompile(`^[a-zA-Z0-9_]+$`)
var kernelRelease = regexp.MustCompile(`^6\.18\.[0-9]+-android17-[0-9]+-[a-zA-Z0-9.-]+$`)

func normalizeModule(name string) string {
	return strings.ReplaceAll(strings.TrimSuffix(filepath.Base(name), ".ko"), "-", "_")
}

func loadPackage(root string) (*kernelPackage, error) {
	data, err := os.ReadFile(filepath.Join(root, "kernel.json"))
	if err != nil {
		return nil, err
	}
	var p kernelPackage
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err = decoder.Decode(&p); err != nil {
		return nil, err
	}
	p.Root = root
	if p.Format != 1 || p.Device != "onyx" || !kernelRelease.MatchString(p.Release) || len(p.Modules) < 300 {
		return nil, fmt.Errorf("unsupported kernel package identity")
	}
	if p.Compression != "zstd,level=9" && p.Compression != "lz4hc,level=12" {
		return nil, fmt.Errorf("unsupported release EROFS compression")
	}
	for name, expected := range p.Hashes {
		if name == "" || !safePath(name) {
			return nil, fmt.Errorf("invalid payload path %q", name)
		}
		path := filepath.Join(root, name)
		info, err := os.Lstat(path)
		if err != nil || !info.Mode().IsRegular() {
			return nil, fmt.Errorf("payload must be a regular file: %s", name)
		}
		actual, err := fileSHA(path)
		if err != nil || actual != expected {
			return nil, fmt.Errorf("payload hash mismatch: %s", name)
		}
	}
	for _, name := range []string{"Image", "dtb.img", "dtbo.img", "modules.builtin", "modules.builtin.modinfo"} {
		if _, ok := p.Hashes["payload/"+name]; !ok {
			return nil, fmt.Errorf("unhashed payload: %s", name)
		}
	}
	builtins := make(map[string]bool)
	for _, name := range p.Builtin {
		if !moduleName.MatchString(name) || builtins[name] {
			return nil, fmt.Errorf("invalid builtin module %q", name)
		}
		builtins[name] = true
	}
	filenames := make(map[string]bool)
	for name, module := range p.Modules {
		if !moduleName.MatchString(name) || normalizeModule(module.File) != name ||
			filepath.Base(module.File) != module.File || !strings.HasSuffix(module.File, ".ko") ||
			(module.Kind != "gki" && module.Kind != "platform") || builtins[name] || filenames[module.File] {
			return nil, fmt.Errorf("invalid module catalog entry %q", name)
		}
		filenames[module.File] = true
		if _, ok := p.Hashes["payload/modules/"+module.File]; !ok {
			return nil, fmt.Errorf("unhashed module %s", name)
		}
		for _, dep := range append(append(append([]string{}, module.Depends...), module.Pre...), module.Post...) {
			if _, ok := p.Modules[dep]; !ok && !builtins[dep] {
				return nil, fmt.Errorf("unresolved dependency %s of %s", dep, name)
			}
		}
		for _, alias := range module.Aliases {
			if alias == "" || strings.ContainsAny(alias, " \t\r\n\x00") {
				return nil, fmt.Errorf("invalid module alias")
			}
		}
	}
	for _, name := range p.NativeAdditions {
		if m, ok := p.Modules[name]; !ok || m.Kind != "gki" {
			return nil, fmt.Errorf("missing native feature module %s", name)
		}
	}
	for _, name := range p.Firmware {
		if !safePath(name) || name == "" {
			return nil, fmt.Errorf("invalid firmware path")
		}
		if _, ok := p.Hashes["payload/firmware/"+name]; !ok {
			return nil, fmt.Errorf("unhashed firmware %s", name)
		}
	}
	if err := p.validateLegacyProfile(); err != nil {
		return nil, err
	}
	return &p, nil
}

// A directory records the ROM's complete flat module inventory, not only the
// modules that happen to have a line in modules.load.
type moduleDirectory struct {
	Names   []string
	Config  map[string][]byte
	Data    map[string][]byte
	Sources map[string]string
}

func (p *kernelPackage) moduleFiles(role string, old moduleDirectory, work string) (map[string]string, error) {
	if len(old.Names) == 0 {
		return nil, fmt.Errorf("%s has no supported module directory", role)
	}
	builtins := make(map[string]bool)
	for _, name := range p.Builtin {
		builtins[name] = true
	}
	oldNames := make(map[string]bool)
	roots := make(map[string]bool)
	retired := make(map[string]bool)
	for _, file := range old.Names {
		name := normalizeModule(file)
		if oldNames[name] {
			return nil, fmt.Errorf("%s has duplicate module identity %s", role, name)
		}
		oldNames[name] = true
		if _, ok := p.Modules[name]; !ok && !builtins[name] {
			if err := p.verifyLegacyModule(old, file); err != nil {
				return nil, fmt.Errorf("%s requires module %s absent from this kernel; ROM is not compatible: %w", role, file, err)
			}
			retired[name] = true
			continue
		}
		roots[name] = true
	}
	loadLists := make(map[string][]string)
	for file, contents := range old.Config {
		if file != "modules.load" && !strings.HasPrefix(file, "modules.load.") {
			continue
		}
		for _, line := range strings.Split(string(contents), "\n") {
			words := strings.Fields(strings.SplitN(line, "#", 2)[0])
			if len(words) == 0 {
				continue
			}
			if len(words) != 1 {
				return nil, fmt.Errorf("unsupported %s/%s load entry %q", role, file, line)
			}
			name := normalizeModule(words[0])
			if retired[name] {
				continue
			}
			if _, ok := p.Modules[name]; !ok && !builtins[name] {
				return nil, fmt.Errorf("%s load list requires absent module %s", role, name)
			}
			roots[name] = true
			if !builtins[name] {
				loadLists[file] = append(loadLists[file], name)
			}
		}
		if _, ok := loadLists[file]; !ok {
			loadLists[file] = nil
		}
	}
	if _, ok := loadLists["modules.load"]; !ok {
		if _, recoveryList := loadLists["modules.load.recovery"]; role != "recovery" || !recoveryList {
			return nil, fmt.Errorf("%s has no supported module load list", role)
		}
	}
	if len(retired) != 0 && role != "system_dlkm" {
		for file := range loadLists {
			profile, ok := p.Legacy.DefaultLoads[role][file]
			if !ok {
				return nil, fmt.Errorf("no reviewed 6.6 migration profile for %s/%s", role, file)
			}
			loadLists[file] = nil
			for _, name := range profile {
				roots[name] = true
				if !builtins[name] {
					loadLists[file] = append(loadLists[file], name)
				}
			}
		}
	}
	var additions []string
	if role != "system_dlkm" {
		for name, module := range p.Modules {
			if module.Kind == "platform" {
				roots[name] = true
				if role == "vendor_dlkm" && !oldNames[name] && len(retired) == 0 {
					additions = append(additions, name)
				}
			}
		}
	}
	if role == "vendor_dlkm" {
		// Keep a complete matching module set available even when another ROM
		// dedicates a much smaller system_dlkm partition to its original modules.
		for name := range p.Modules {
			roots[name] = true
		}
		for _, name := range p.NativeAdditions {
			roots[name] = true
			additions = append(additions, name)
		}
	}
	sort.Strings(additions)
	requested := make(map[string]bool)
	for _, name := range loadLists["modules.load"] {
		requested[name] = true
	}
	for _, name := range additions {
		if !requested[name] {
			loadLists["modules.load"] = append(loadLists["modules.load"], name)
			requested[name] = true
		}
	}
	selected, visiting := make(map[string]bool), make(map[string]bool)
	var add func(string) error
	add = func(name string) error {
		if selected[name] || builtins[name] {
			return nil
		}
		if visiting[name] {
			return fmt.Errorf("hard dependency cycle at %s", name)
		}
		visiting[name] = true
		for _, dep := range p.Modules[name].Depends {
			if err := add(dep); err != nil {
				return err
			}
		}
		delete(visiting, name)
		selected[name] = true
		for _, dep := range append(append([]string{}, p.Modules[name].Pre...), p.Modules[name].Post...) {
			if !visiting[dep] {
				if err := add(dep); err != nil {
					return err
				}
			}
		}
		return nil
	}
	for name := range roots {
		if err := add(name); err != nil {
			return nil, err
		}
	}
	names := make([]string, 0, len(selected))
	for name := range selected {
		names = append(names, name)
	}
	sort.Strings(names)
	files := make(map[string]string)
	var deps, aliases, softdeps, order strings.Builder
	for _, name := range names {
		module := p.Modules[name]
		files[module.File] = filepath.Join(p.Root, "payload/modules", module.File)
		order.WriteString(module.File + "\n")
		deps.WriteString(module.File + ":")
		// Android libmodprobe expects immediate dependencies and recursively
		// resolves them using this complete per-partition table.
		for _, dep := range module.Depends {
			if selected[dep] {
				deps.WriteString(" " + p.Modules[dep].File)
			}
		}
		deps.WriteByte('\n')
		for _, alias := range module.Aliases {
			aliases.WriteString("alias " + alias + " " + name + "\n")
		}
		if len(module.Pre)+len(module.Post) != 0 {
			softdeps.WriteString("softdep " + name)
			for i, group := range [][]string{module.Pre, module.Post} {
				if len(group) == 0 {
					continue
				}
				softdeps.WriteString([]string{" pre:", " post:"}[i])
				for _, dep := range group {
					softdeps.WriteString(" " + dep)
				}
			}
			softdeps.WriteByte('\n')
		}
	}
	generated := map[string][]byte{"modules.dep": []byte(deps.String()), "modules.alias": []byte(aliases.String()),
		"modules.softdep": []byte(softdeps.String()), "modules.order": []byte(order.String())}
	for file, list := range loadLists {
		var contents strings.Builder
		for _, name := range list {
			contents.WriteString(p.Modules[name].File + "\n")
		}
		generated[file] = []byte(contents.String())
	}
	for file, contents := range old.Config {
		// Keep ROM-specific module parameters, blacklists, and other configuration.
		// Stale generated binary indexes must never describe the replaced .ko files.
		if _, replaced := generated[file]; replaced || file == "modules.builtin" || file == "modules.builtin.modinfo" ||
			file == "modules.symbols" || strings.HasSuffix(file, ".bin") {
			continue
		}
		generated[file] = contents
	}
	if err := os.Mkdir(work, 0700); err != nil {
		return nil, err
	}
	for file, contents := range generated {
		path := filepath.Join(work, file)
		if err := os.WriteFile(path, contents, 0600); err != nil {
			return nil, err
		}
		files[file] = path
	}
	for _, name := range []string{"modules.builtin", "modules.builtin.modinfo"} {
		files[name] = filepath.Join(p.Root, "payload", name)
	}
	fmt.Printf("%s: prepared %d matched modules\n", role, len(selected))
	if role == "vendor_ramdisk" && len(loadLists["modules.load.recovery"]) > 0 {
		p.VendorRecovery = true
	}
	if len(retired) != 0 {
		if p.Retired == nil {
			p.Retired = make(map[string][]string)
		}
		for name := range retired {
			p.Retired[role] = append(p.Retired[role], name)
		}
		sort.Strings(p.Retired[role])
		fmt.Printf("%s: migrated %d verified legacy Onyx 6.6 modules\n", role, len(retired))
	}
	return files, nil
}

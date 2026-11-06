// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func golden(t *testing.T, name string) []byte {
	t.Helper()
	file, err := os.Open(filepath.Join("testdata", name+".gz"))
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	r, err := gzip.NewReader(file)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	data, err := io.ReadAll(r)
	if err != nil {
		t.Fatal(err)
	}
	meta, err := os.ReadFile(filepath.Join("testdata", name+".json"))
	if err != nil {
		t.Fatal(err)
	}
	var provenance struct {
		SHA256 string `json:"sha256"`
	}
	if err = json.Unmarshal(meta, &provenance); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(data)
	if hex.EncodeToString(sum[:]) != provenance.SHA256 {
		t.Fatal("independent fixture changed")
	}
	return data
}

func TestAndroidBootImageFixtures(t *testing.T) {
	for _, name := range []string{"boot-v4", "vendor-v4"} {
		t.Run(name, func(t *testing.T) {
			data := golden(t, name)
			parsed, err := parseBoot(data)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(parsed.encode(), data) {
				t.Fatal("unchanged Android boot image did not roundtrip exactly")
			}
			if string(parsed.Ramdisk) != "original ramdisk payload" {
				t.Fatal("incorrect ramdisk offsets")
			}
			if parsed.Vendor {
				if string(parsed.DTB) != "original device tree" || !strings.Contains(string(parsed.Bootconfig), `androidboot.load_modules_parallel = "true"`) {
					t.Fatal("vendor metadata changed")
				}
			} else if string(parsed.Kernel) != "original kernel payload" {
				t.Fatal("incorrect kernel offsets")
			}
			bad := append([]byte{}, data...)
			if parsed.Vendor {
				le.PutUint32(bad[24:], 0xffffffff)
			} else {
				le.PutUint32(bad[8:], 0xffffffff)
			}
			if _, err = parseBoot(bad); err == nil {
				t.Fatal("accepted a section past the end of the image")
			}
		})
	}
}

func TestBootParameterUpdateKeepsROMChoices(t *testing.T) {
	field := make([]byte, 2048)
	copy(field, "console=ttyMSM0 androidboot.load_modules_parallel=true android_arch_task_struct_size=384 sched_proxy_exec=1 firmware_class.path=/vendor/firmware")
	if err := commandLine(field, []string{"android_arch_task_struct_size=512", "sched_proxy_exec=0"}, true, true); err != nil {
		t.Fatal(err)
	}
	line := string(bytes.TrimRight(field, "\x00"))
	for _, want := range []string{"console=ttyMSM0", "androidboot.load_modules_parallel=true", "android_arch_task_struct_size=512", "sched_proxy_exec=0", "firmware_class.path=/vendor_dlkm/lib/firmware,/vendor/firmware"} {
		if !strings.Contains(line, want) {
			t.Fatalf("missing %s in %s", want, line)
		}
	}
	before := append([]byte{}, field...)
	if err := commandLine(field, []string{"android_arch_task_struct_size=512", "sched_proxy_exec=0"}, true, true); err != nil || !bytes.Equal(field, before) {
		t.Fatal("command line update is not idempotent", err)
	}
}

func TestInitOverridesRequireAnExplicitROMMigration(t *testing.T) {
	for _, text := range []string{"androidboot.init_rc=/custom.rc", "androidboot.init_rc = \"/custom.rc\"", "other=true\nandroidboot.init_rc = \"/custom.rc\""} {
		if !initOverride.MatchString(text) {
			t.Fatal("bootconfig override was missed", text)
		}
		field := make([]byte, 2048)
		copy(field, text)
		if err := commandLine(field, nil, true, false); err == nil {
			t.Fatal("command-line override was accepted")
		}
	}
}

func fixturePackage(t *testing.T) *kernelPackage {
	t.Helper()
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "payload/modules"), 0700); err != nil {
		t.Fatal(err)
	}
	p := &kernelPackage{Root: root, Release: "6.18.38-android17-6-test", Builtin: []string{"compiled_in"}, NativeAdditions: []string{"feature"}, Modules: map[string]kernelModule{
		"driver":     {File: "driver.ko", Kind: "platform", Depends: []string{"dependency"}, Pre: []string{"compiled_in"}},
		"dependency": {File: "dependency.ko", Kind: "gki"}, "feature": {File: "feature.ko", Kind: "gki"}, "other": {File: "other.ko", Kind: "gki"}}}
	for _, m := range p.Modules {
		if err := os.WriteFile(filepath.Join(root, "payload/modules", m.File), []byte("new "+m.File), 0600); err != nil {
			t.Fatal(err)
		}
	}
	for _, name := range []string{"modules.builtin", "modules.builtin.modinfo"} {
		if err := os.WriteFile(filepath.Join(root, "payload", name), []byte("compiled in\n"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	return p
}

func TestModuleMigrationPreservesConfigurationAndHasFullCoverage(t *testing.T) {
	p := fixturePackage(t)
	old := moduleDirectory{Names: []string{"driver.ko"}, Config: map[string][]byte{"modules.load": []byte("driver.ko\ncompiled-in.ko\n"), "modules.options": []byte("options driver firmware=test\n"), "modules.blocklist": []byte("blocklist dependency\n"), "modules.dep.bin": []byte("stale")}}
	files, err := p.moduleFiles("vendor_dlkm", old, filepath.Join(t.TempDir(), "generated"))
	if err != nil {
		t.Fatal(err)
	}
	for _, module := range p.Modules {
		if _, ok := files[module.File]; !ok {
			t.Fatalf("compiled module omitted: %s", module.File)
		}
	}
	for _, name := range []string{"modules.options", "modules.blocklist"} {
		got, err := os.ReadFile(files[name])
		if err != nil || !bytes.Equal(got, old.Config[name]) {
			t.Fatalf("ROM configuration changed: %s", name)
		}
	}
	if _, ok := files["modules.dep.bin"]; ok {
		t.Fatal("stale binary dependency index retained")
	}
	load, _ := os.ReadFile(files["modules.load"])
	if string(load) != "driver.ko\nfeature.ko\n" {
		t.Fatalf("unexpected load order %q", load)
	}
	deps, _ := os.ReadFile(files["modules.dep"])
	if !strings.Contains(string(deps), "driver.ko: dependency.ko\n") {
		t.Fatal("module dependency missing")
	}
	old.Names = append(old.Names, "rom_only_driver.ko")
	if _, err = p.moduleFiles("vendor_dlkm", old, filepath.Join(t.TempDir(), "rejected")); err == nil {
		t.Fatal("unknown ROM module was silently removed")
	}
}

func TestLegacyMigrationVerifiesBinaryAndPreservesSmallSystemSelection(t *testing.T) {
	for _, role := range []string{"vendor_dlkm", "system_dlkm", "recovery"} {
		t.Run(role, func(t *testing.T) {
			p := fixturePackage(t)
			file := "modules.load"
			if role == "recovery" {
				file = "modules.load.recovery"
			}
			p.Legacy = &legacyProfile{ModuleNames: []string{"legacy_driver"}, DefaultLoads: map[string]map[string][]string{
				role: {file: {"driver"}},
			}}
			p.Modules["early_only"] = kernelModule{File: "early_only.ko", Kind: "platform"}
			if err := p.validateLegacyProfile(); err != nil {
				t.Fatal(err)
			}
			old := moduleDirectory{Names: []string{"dependency.ko", "legacy-driver.ko"},
				Data: map[string][]byte{"legacy-driver.ko": golden(t, "legacy-module")},
				Config: map[string][]byte{file: []byte("dependency.ko\nlegacy-driver.ko\n"),
					"modules.options":   []byte("options driver parameter=preserved\n"),
					"modules.blocklist": []byte("blocklist other\n")}}
			// Exercise disk-backed metadata too, as used for EROFS partitions.
			if role == "system_dlkm" {
				path := filepath.Join(t.TempDir(), "legacy-driver.ko")
				if err := os.WriteFile(path, old.Data["legacy-driver.ko"], 0600); err != nil {
					t.Fatal(err)
				}
				old.Data = nil
				old.Sources = map[string]string{"legacy-driver.ko": path}
			}
			files, err := p.moduleFiles(role, old, filepath.Join(t.TempDir(), "modules"))
			if err != nil {
				t.Fatal(err)
			}
			load, err := os.ReadFile(files[file])
			if err != nil {
				t.Fatal(err)
			}
			if role == "system_dlkm" {
				if string(load) != "dependency.ko\n" || files["driver.ko"] != "" || files["feature.ko"] != "" {
					t.Fatal("small system_dlkm selection was expanded", string(load))
				}
			} else if !strings.HasPrefix(string(load), "driver.ko\n") {
				t.Fatal("validated replacement load profile not used", string(load))
			}
			if strings.Contains(string(load), "early_only") {
				t.Fatal("migration loaded a platform driver outside the reviewed load profile")
			}
			if files["legacy-driver.ko"] != "" || len(p.Retired[role]) != 1 || p.Retired[role][0] != "legacy_driver" {
				t.Fatal("retired module inventory is incorrect")
			}
			for _, name := range []string{"modules.options", "modules.blocklist"} {
				got, _ := os.ReadFile(files[name])
				if !bytes.Equal(got, old.Config[name]) {
					t.Fatal("ROM module configuration changed")
				}
			}
		})
	}
}

func TestLegacyNamesCannotAuthorizeAnUnverifiedModule(t *testing.T) {
	good := golden(t, "legacy-module")
	cases := map[string][]byte{
		"not_elf":          []byte("name=legacy_driver\x00vermagic=6.6.118-fixture\x00"),
		"different_kernel": bytes.ReplaceAll(good, []byte("6.6.118"), []byte("6.5.118")),
		"different_name":   bytes.ReplaceAll(good, []byte("name=legacy_driver"), []byte("name=custom_driver")),
		"missing_binary":   nil,
	}
	wrongArch := append([]byte{}, good...)
	le.PutUint16(wrongArch[18:], 62) // EM_X86_64
	cases["wrong_architecture"] = wrongArch
	for name, data := range cases {
		t.Run(name, func(t *testing.T) {
			p := fixturePackage(t)
			p.Legacy = &legacyProfile{ModuleNames: []string{"legacy_driver"}}
			old := moduleDirectory{Names: []string{"legacy-driver.ko"}, Data: map[string][]byte{"legacy-driver.ko": data},
				Config: map[string][]byte{"modules.load": []byte("legacy-driver.ko\n")}}
			if _, err := p.moduleFiles("system_dlkm", old, filepath.Join(t.TempDir(), "rejected")); err == nil {
				t.Fatal("unverified legacy module was removed")
			}
		})
	}
	p := fixturePackage(t)
	p.Legacy = &legacyProfile{ModuleNames: []string{"legacy_driver"}}
	old := moduleDirectory{Names: []string{"dependency.ko"}, Config: map[string][]byte{"modules.load": []byte("legacy-driver.ko\n")}}
	if _, err := p.moduleFiles("system_dlkm", old, filepath.Join(t.TempDir(), "rejected")); err == nil {
		t.Fatal("load-list entry without a verified binary was retired")
	}
}

func TestCPIOKeepsNonModuleDataAndRejectsTraversal(t *testing.T) {
	p := fixturePackage(t)
	var entries []cpioEntry
	add := func(name string, mode uint32, data string) {
		e := cpioEntry{Name: name, Magic: "070701", Data: []byte(data)}
		e.Fields[0] = uint32(len(entries) + 1)
		e.Fields[1] = mode
		e.Fields[2] = 1001
		e.Fields[3] = 3003
		e.Fields[4] = 1
		e.Fields[5] = 12345678
		entries = append(entries, e)
	}
	for _, dir := range []string{".", "lib", "lib/modules", "etc", "dev"} {
		add(dir, 0040755, "")
	}
	add("lib/modules/driver.ko", 0100644, "old module")
	add("lib/modules/modules.load.recovery", 0100644, "driver.ko\n")
	add("etc/rom.rc", 0100600, "service from this ROM\x00\xff")
	add("dev/console", 0020600, "")
	entries[len(entries)-1].Fields[9] = 5
	entries[len(entries)-1].Fields[10] = 1
	data, err := writeCPIO(entries)
	if err != nil {
		t.Fatal(err)
	}
	updated, changed, err := p.replaceCPIO(data, "recovery", filepath.Join(t.TempDir(), "modules"))
	if err != nil || !changed {
		t.Fatal("ramdisk migration failed", err)
	}
	decoded, err := readCPIO(updated)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range entries {
		if strings.HasPrefix(want.Name, "lib/modules/") {
			continue
		}
		found := false
		for _, got := range decoded {
			if got.Name == want.Name {
				found = true
				want.Fields[6], want.Fields[11] = got.Fields[6], got.Fields[11]
				if want.Fields != got.Fields || !bytes.Equal(want.Data, got.Data) {
					t.Fatalf("ROM ramdisk entry changed: %s", want.Name)
				}
			}
		}
		if !found {
			t.Fatalf("ROM entry lost: %s", want.Name)
		}
	}
	entries[0].Name = "../../escape"
	bad, err := writeCPIO(entries)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = readCPIO(bad); err == nil {
		t.Fatal("accepted traversal in ramdisk")
	}
}

func TestLibLPFixtureAndSharedSlotRejection(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "super.img")
	data := golden(t, "liblp-metadata")
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	for i, name := range []string{"vendor_dlkm", "system_dlkm"} {
		text := fmt.Sprintf("Mapping: %s_a 253:7 event=0 uuid=\n0 4096 linear 8:1 %d\n", name, 2048+i*4096)
		if err := os.WriteFile(filepath.Join(root, name+"-map.txt"), []byte(text), 0600); err != nil {
			t.Fatal(err)
		}
	}
	if err := checkLayout(path, "_a", root); err != nil {
		t.Fatal(err)
	}
	// Create checksum-valid metadata where inactive vendor_dlkm_b points at the
	// active partition's physical storage. This must fail even with no snapshots.
	for _, offset := range []int{12288, 12288 + 2*65536} {
		header := data[offset : offset+65536]
		hsize, tsize := int(le.Uint32(header[8:])), int(le.Uint32(header[44:]))
		extents := int(le.Uint32(header[92:]))
		le.PutUint64(header[hsize+extents+2*24+12:], 2048)
		sum := sha256.Sum256(header[hsize : hsize+tsize])
		copy(header[48:80], sum[:])
		clear(header[12:44])
		sum = sha256.Sum256(header[:hsize])
		copy(header[12:44], sum[:])
	}
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	if err := checkLayout(path, "_a", root); err == nil || !strings.Contains(err.Error(), "shares physical data") {
		t.Fatalf("shared slot was accepted: %v", err)
	}
	data[4096+40] ^= 1
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := readLP(path, 0); err == nil {
		t.Fatal("corrupt geometry was accepted")
	}
}

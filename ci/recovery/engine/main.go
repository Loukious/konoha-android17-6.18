// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"fmt"
	"os"
	"path/filepath"
)

func main() {
	if len(os.Args) == 5 && os.Args[1] == "check-layout" {
		if err := checkLayout(os.Args[2], os.Args[3], os.Args[4]); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	if len(os.Args) == 4 && os.Args[1] == "extract" {
		if err := extractPackage(os.Args[2], os.Args[3]); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	if len(os.Args) == 6 && os.Args[1] == "prepare" {
		if err := prepareImages(os.Args[2], os.Args[3], os.Args[4], os.Args[5]); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		fmt.Println("All six images prepared and verified; no partition has been written")
		return
	}
	if len(os.Args) != 6 || os.Args[1] != "check-erofs-roundtrip" {
		fmt.Fprintln(os.Stderr, "usage: onyx-recovery prepare PACKAGE INPUT OUTPUT WORK\n       onyx-recovery check-erofs-roundtrip TOOLS INPUT OUTPUT WORK")
		os.Exit(2)
	}
	tools, input, output, work := os.Args[2], os.Args[3], os.Args[4], os.Args[5]
	err := os.Mkdir(work, 0700)
	if err == nil {
		var tree *imageTree
		tree, err = readEROFS(tools, input, filepath.Join(work, "original"))
		if err == nil {
			err = writeEROFS(tools, tree, output, work)
		}
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println("EROFS data, metadata, xattrs and hardlinks preserved")
}

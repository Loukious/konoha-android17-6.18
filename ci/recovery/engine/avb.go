// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"bytes"
	"crypto/sha1"
	"crypto/sha256"
	"crypto/sha512"
	"encoding/binary"
	"fmt"
	"hash"
	"io"
	"os"
	"strings"
)

var be = binary.BigEndian

type avbImage struct {
	Capacity     int64
	OriginalSize uint64
	Header       []byte
	Descriptors  [][]byte
}

func aligned(n, block uint64) uint64 { return (n + block - 1) / block * block }

func readAVB(path string) (*avbImage, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	stat, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if !stat.Mode().IsRegular() || stat.Size() < 4096 {
		return nil, fmt.Errorf("invalid AVB image %s", path)
	}
	footer := make([]byte, 64)
	if _, err = f.ReadAt(footer, stat.Size()-64); err != nil {
		return nil, err
	}
	if string(footer[:4]) != "AVBf" || be.Uint32(footer[4:]) != 1 || be.Uint32(footer[8:]) != 0 {
		return nil, fmt.Errorf("unsupported AVB footer: %s", path)
	}
	original, offset, size := be.Uint64(footer[12:]), be.Uint64(footer[20:]), be.Uint64(footer[28:])
	limit := uint64(stat.Size() - 64)
	if original > offset || offset > limit || size < 256 || size > 65536 || size > limit-offset {
		return nil, fmt.Errorf("invalid AVB image bounds")
	}
	meta := make([]byte, size)
	if _, err = f.ReadAt(meta, int64(offset)); err != nil {
		return nil, err
	}
	if string(meta[:4]) != "AVB0" || be.Uint32(meta[4:]) != 1 || be.Uint32(meta[8:]) > 3 {
		return nil, fmt.Errorf("unsupported AVB metadata version")
	}
	auth, aux := be.Uint64(meta[12:]), be.Uint64(meta[20:])
	doff, dsize := be.Uint64(meta[96:]), be.Uint64(meta[104:])
	if auth > size-256 || aux != size-256-auth || doff > aux || dsize > aux-doff {
		return nil, fmt.Errorf("invalid AVB descriptor bounds")
	}
	result := &avbImage{Capacity: stat.Size(), OriginalSize: original, Header: append([]byte(nil), meta[:256]...)}
	descriptors := meta[256+auth+doff : 256+auth+doff+dsize]
	for len(descriptors) != 0 {
		if len(descriptors) < 16 {
			return nil, fmt.Errorf("truncated AVB descriptor")
		}
		n := be.Uint64(descriptors[8:])
		if n > uint64(len(descriptors)-16) || n%8 != 0 {
			return nil, fmt.Errorf("invalid AVB descriptor length")
		}
		result.Descriptors = append(result.Descriptors, append([]byte(nil), descriptors[:16+n]...))
		descriptors = descriptors[16+n:]
	}
	return result, nil
}

func digestAlgorithm(name string) (func() hash.Hash, error) {
	switch name {
	case "sha1":
		return sha1.New, nil
	case "sha256":
		return sha256.New, nil
	case "sha512":
		return sha512.New, nil
	default:
		return nil, fmt.Errorf("unsupported AVB digest %q", name)
	}
}

func descriptorInputs(desc []byte, partition string) (algorithm string, salt []byte, err error) {
	var fixed, algorithmOffset, nameOffset int
	switch be.Uint64(desc) {
	case 1:
		fixed, algorithmOffset, nameOffset = 180, 72, 104
	case 2:
		fixed, algorithmOffset, nameOffset = 132, 24, 56
	default:
		return "", nil, fmt.Errorf("not an AVB image descriptor")
	}
	if len(desc) < fixed {
		return "", nil, fmt.Errorf("short AVB image descriptor")
	}
	n, s, d := uint64(be.Uint32(desc[nameOffset:])), uint64(be.Uint32(desc[nameOffset+4:])), uint64(be.Uint32(desc[nameOffset+8:]))
	if n == 0 || s > 1024 || d > 64 || aligned(uint64(fixed)+n+s+d, 8) != uint64(len(desc)) {
		return "", nil, fmt.Errorf("invalid AVB image descriptor fields")
	}
	if string(desc[fixed:uint64(fixed)+n]) != partition {
		return "", nil, fmt.Errorf("AVB partition name differs from %s", partition)
	}
	algorithm = strings.TrimRight(string(desc[algorithmOffset:algorithmOffset+32]), "\x00")
	if _, err = digestAlgorithm(algorithm); err != nil {
		return "", nil, err
	}
	return algorithm, append([]byte(nil), desc[uint64(fixed)+n:uint64(fixed)+n+s]...), nil
}

func makeHashTree(file *os.File, size, block uint64, algorithm string, salt []byte) ([]byte, []byte, error) {
	newHash, err := digestAlgorithm(algorithm)
	if err != nil {
		return nil, nil, err
	}
	if block < 512 || block > 4096 || block&(block-1) != 0 || size == 0 || size%block != 0 {
		return nil, nil, fmt.Errorf("invalid AVB hashtree block layout")
	}
	digestSize := newHash().Size()
	paddedDigest := 1
	for paddedDigest < digestSize {
		paddedDigest *= 2
	}
	var levels [][]byte
	var previous []byte
	currentSize := size
	for currentSize > block {
		next := make([]byte, aligned(currentSize/block*uint64(paddedDigest), block))
		buffer := make([]byte, block)
		for offset := uint64(0); offset < currentSize; offset += block {
			if previous == nil {
				if _, err = file.ReadAt(buffer, int64(offset)); err != nil {
					return nil, nil, err
				}
			} else {
				copy(buffer, previous[offset:offset+block])
			}
			h := newHash()
			h.Write(salt)
			h.Write(buffer)
			copy(next[offset/block*uint64(paddedDigest):], h.Sum(nil))
		}
		levels = append(levels, next)
		previous = next
		currentSize = uint64(len(next))
	}
	if previous == nil {
		previous = make([]byte, block)
		if _, err = file.ReadAt(previous, 0); err != nil {
			return nil, nil, err
		}
	}
	h := newHash()
	h.Write(salt)
	h.Write(previous)
	var tree []byte
	for n := len(levels) - 1; n >= 0; n-- {
		tree = append(tree, levels[n]...)
	}
	return tree, h.Sum(nil), nil
}

// Rebuild local image metadata. The installer must separately enforce the
// existing unlocked/disabled-verification prerequisite before using NONE.
func writeAVB(source, target, partition string) error {
	original, err := readAVB(source)
	if err != nil {
		return err
	}
	output, err := os.OpenFile(target, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	defer output.Close()
	info, err := output.Stat()
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() || info.Size() <= 0 || info.Size() >= original.Capacity-4096 {
		return fmt.Errorf("rebuilt %s image exceeds partition capacity", partition)
	}
	payloadSize := uint64(info.Size())
	var descriptors []byte
	var tree []byte
	images := 0
	for _, old := range original.Descriptors {
		tag := be.Uint64(old)
		if tag == 0 {
			descriptors = append(descriptors, old...)
			continue
		}
		if tag != 1 && tag != 2 {
			return fmt.Errorf("unsupported dependent AVB descriptor %d in %s", tag, partition)
		}
		images++
		algorithm, salt, err := descriptorInputs(old, partition)
		if err != nil {
			return err
		}
		newHash, _ := digestAlgorithm(algorithm)
		var digest []byte
		var desc []byte
		if tag == 2 {
			h := newHash()
			h.Write(salt)
			if _, err = io.Copy(h, io.NewSectionReader(output, 0, int64(payloadSize))); err != nil {
				return err
			}
			digest = h.Sum(nil)
			desc = append([]byte(nil), old[:132]...)
			be.PutUint64(desc[16:], payloadSize)
			be.PutUint32(desc[64:], uint32(len(digest)))
		} else {
			dataBlock, hashBlock := uint64(be.Uint32(old[44:])), uint64(be.Uint32(old[48:]))
			if dataBlock != hashBlock || be.Uint32(old[16:]) != 1 {
				return fmt.Errorf("unsupported AVB hashtree format")
			}
			padded := aligned(payloadSize, dataBlock)
			if err = output.Truncate(int64(padded)); err != nil {
				return err
			}
			tree, digest, err = makeHashTree(output, padded, dataBlock, algorithm, salt)
			if err != nil {
				return err
			}
			desc = append([]byte(nil), old[:180]...)
			be.PutUint64(desc[20:], padded)
			be.PutUint64(desc[28:], padded)
			be.PutUint64(desc[36:], uint64(len(tree)))
			be.PutUint32(desc[52:], 0)
			be.PutUint64(desc[56:], 0)
			be.PutUint64(desc[64:], 0)
			be.PutUint32(desc[112:], uint32(len(digest)))
			payloadSize = padded
		}
		desc = append(desc, []byte(partition)...)
		desc = append(desc, salt...)
		desc = append(desc, digest...)
		desc = append(desc, make([]byte, aligned(uint64(len(desc)), 8)-uint64(len(desc)))...)
		be.PutUint64(desc[8:], uint64(len(desc)-16))
		descriptors = append(descriptors, desc...)
	}
	if images != 1 {
		return fmt.Errorf("expected one AVB image descriptor for %s", partition)
	}
	header := append([]byte(nil), original.Header...)
	clear(header[12:112]) // NONE has no auth, signature or public-key data.
	auxSize := aligned(uint64(len(descriptors)), 64)
	be.PutUint64(header[20:], auxSize)
	be.PutUint64(header[104:], uint64(len(descriptors)))
	clear(header[128:176])
	copy(header[128:176], "onyx-recovery")
	metadata := append(header, descriptors...)
	metadata = append(metadata, make([]byte, 256+auxSize-uint64(len(metadata)))...)
	metaOffset := aligned(payloadSize+uint64(len(tree)), 4096)
	if metaOffset+uint64(len(metadata)) > uint64(original.Capacity-4096) {
		return fmt.Errorf("rebuilt %s image and AVB metadata exceed partition capacity", partition)
	}
	if len(tree) != 0 {
		if _, err = output.WriteAt(tree, int64(payloadSize)); err != nil {
			return err
		}
	}
	if _, err = output.WriteAt(metadata, int64(metaOffset)); err != nil {
		return err
	}
	footer := make([]byte, 64)
	copy(footer, "AVBf")
	be.PutUint32(footer[4:], 1)
	be.PutUint64(footer[12:], uint64(info.Size()))
	be.PutUint64(footer[20:], metaOffset)
	be.PutUint64(footer[28:], uint64(len(metadata)))
	if err = output.Truncate(original.Capacity); err != nil {
		return err
	}
	if _, err = output.WriteAt(footer, original.Capacity-64); err != nil {
		return err
	}
	if err = output.Sync(); err != nil {
		return err
	}
	check, err := readAVB(target)
	if err != nil {
		return err
	}
	if check.Capacity != original.Capacity || !bytes.Equal(check.Header[112:128], original.Header[112:128]) {
		return fmt.Errorf("AVB capacity or rollback metadata changed")
	}
	return nil
}

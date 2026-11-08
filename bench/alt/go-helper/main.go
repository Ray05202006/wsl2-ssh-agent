// Measurement prototype only: the Windows helper (one connection) in Go.
package main

import (
	"encoding/binary"
	"io"
	"os"
	"syscall"
	"unsafe"
)

func readMsg(r io.Reader, buf []byte) (int, error) {
	if _, err := io.ReadFull(r, buf[:4]); err != nil {
		return 0, err
	}
	n := binary.BigEndian.Uint32(buf)
	if n == 0 || n > 256*1024 {
		return 0, io.ErrUnexpectedEOF
	}
	_, err := io.ReadFull(r, buf[4:4+n])
	return int(4 + n), err
}

func main() {
	name, _ := syscall.UTF16PtrFromString(`\\.\pipe\openssh-ssh-agent`)
	h, err := syscall.CreateFile(name, syscall.GENERIC_READ|syscall.GENERIC_WRITE, 0, nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		os.Exit(2)
	}
	_ = unsafe.Sizeof(h)
	p := os.NewFile(uintptr(h), "pipe")
	buf := make([]byte, 4+256*1024)
	for {
		n, err := readMsg(os.Stdin, buf)
		if err != nil {
			return
		}
		p.Write(buf[:n])
		if n, err = readMsg(p, buf); err != nil {
			os.Exit(3)
		}
		os.Stdout.Write(buf[:n])
	}
}

// Measurement prototype only: the per-connection spawn design in Go.
package main

import (
	"net"
	"os"
	"os/exec"
)

func main() {
	sock, helper := os.Args[1], os.Args[2]
	os.Remove(sock)
	l, err := net.Listen("unix", sock)
	if err != nil {
		panic(err)
	}
	for {
		c, err := l.Accept()
		if err != nil {
			continue
		}
		f, _ := c.(*net.UnixConn).File()
		c.Close()
		cmd := exec.Command(helper, "openssh-ssh-agent")
		cmd.Stdin, cmd.Stdout = f, f
		if cmd.Start() == nil {
			go cmd.Wait()
		}
		f.Close()
	}
}

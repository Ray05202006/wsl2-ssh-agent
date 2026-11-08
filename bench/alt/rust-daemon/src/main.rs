// Measurement prototype only: the per-connection spawn design in Rust (std only).
use std::os::fd::OwnedFd;
use std::os::unix::net::UnixListener;
use std::process::{Command, Stdio};

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let _ = std::fs::remove_file(&a[1]);
    let l = UnixListener::bind(&a[1]).unwrap();
    unsafe { libc_ignore_sigchld() };
    for c in l.incoming().flatten() {
        let fd: OwnedFd = c.into();
        let fd2 = fd.try_clone().unwrap();
        let _ = Command::new(&a[2]).arg("openssh-ssh-agent")
            .stdin(Stdio::from(fd)).stdout(Stdio::from(fd2)).spawn();
    }
}

// SIGCHLD = SIG_IGN: kernel auto-reaps children (no libc crate needed)
unsafe fn libc_ignore_sigchld() {
    extern "C" { fn signal(sig: i32, h: usize) -> usize; }
    signal(17, 1);
}
